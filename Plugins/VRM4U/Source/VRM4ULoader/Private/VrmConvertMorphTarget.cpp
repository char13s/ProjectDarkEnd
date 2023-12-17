// VRM4U Copyright (c) 2021-2023 Haruyoshi Yamamoto. This software is released under the MIT License.

#include "VrmConvertMorphTarget.h"
#include "VrmConvert.h"

#include "VrmAssetListObject.h"
#include "LoaderBPFunctionLibrary.h"
#include "VRM4ULoaderLog.h"

#include "Engine/SkeletalMesh.h"
#include "RenderingThread.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Animation/MorphTarget.h"

#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"

#include "Async/ParallelFor.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/mesh.h>
#include <assimp/postprocess.h>
#include <assimp/GltfMaterial.h>
#include <assimp/vrm/vrmmeta.h>

namespace {
#if WITH_EDITOR
	void LocalPopulateDeltas(UMorphTarget* Morph, const TArray<FMorphTargetDelta>& Deltas, const int32 LODIndex, const TArray<struct FSkelMeshSection>& Sections)
	{
		Morph->PopulateDeltas(Deltas, LODIndex, Sections);
	}
#else
	void LocalPopulateDeltas(USkeletalMesh* sk, UMorphTarget * Morph, const TArray<FMorphTargetDelta> & Deltas, const int32 LODIndex, const bool bCompareNormal = false, const bool bGeneratedByReductionSetting = false, const float PositionThreshold = THRESH_POINTS_ARE_NEAR)
	{

		//TIndirectArray<FSkeletalMeshLODRenderData> LODRenderData;
		const auto& Sections = sk->GetResourceForRendering()->LODRenderData[0].RenderSections;

		//FSkeletalMeshLODRenderData& rd = sk->GetResourceForRendering()->LODRenderData[0];

#if	UE_VERSION_OLDER_THAN(5,0,0)
		auto& MorphLODModels = Morph->MorphLODModels;
#else
		auto& MorphLODModels = Morph->GetMorphLODModels();
#endif


		// create the LOD entry if it doesn't already exist
		if (LODIndex >= MorphLODModels.Num())
		{
			MorphLODModels.AddDefaulted(LODIndex - MorphLODModels.Num() + 1);
		}

		// morph mesh data to modify
		FMorphTargetLODModel& MorphModel = MorphLODModels[LODIndex];
		// copy the wedge point indices
		// for now just keep every thing 

		// set the original number of vertices
		MorphModel.NumBaseMeshVerts = Deltas.Num();

		// empty morph mesh vertices first
		MorphModel.Vertices.Empty(Deltas.Num());

		// mark if generated by reduction setting, so that we can remove them later if we want to
		// we don't want to delete if it has been imported
		MorphModel.bGeneratedByEngine = bGeneratedByReductionSetting;

		// Still keep this (could remove in long term due to incoming data)
		for (const FMorphTargetDelta& Delta : Deltas)
		{
			if (Delta.PositionDelta.SizeSquared() > FMath::Square(PositionThreshold) ||
				(bCompareNormal && Delta.TangentZDelta.SizeSquared() > 0.01f))
			{
				MorphModel.Vertices.Add(Delta);
				for (int32 SectionIdx = 0; SectionIdx < Sections.Num(); ++SectionIdx)
				{
					if (MorphModel.SectionIndices.Contains(SectionIdx))
					{
						continue;
					}
					//Sections[0].BaseVertexIndex
					//const uint32 BaseVertexBufferIndex = (uint32)(Sections[SectionIdx].GetVertexBufferIndex());
					//const uint32 LastVertexBufferIndex = (uint32)(BaseVertexBufferIndex + Sections[SectionIdx].GetNumVertices());
#if	UE_VERSION_OLDER_THAN(5,0,0)
					const uint32 BaseVertexBufferIndex = (uint32)(Sections[SectionIdx].BaseVertexIndex);
					const uint32 LastVertexBufferIndex = (uint32)(BaseVertexBufferIndex + Sections[SectionIdx].NumVertices);
#else
					const uint32 BaseVertexBufferIndex = (uint32)(Sections[SectionIdx].GetVertexBufferIndex());
					const uint32 LastVertexBufferIndex = (uint32)(BaseVertexBufferIndex + Sections[SectionIdx].GetNumVertices());
#endif
					if (BaseVertexBufferIndex <= Delta.SourceIdx && Delta.SourceIdx < LastVertexBufferIndex)
					{
						MorphModel.SectionIndices.AddUnique(SectionIdx);
						break;
					}
				}
			}
		}

		// sort the array of vertices for this morph target based on the base mesh indices
		// that each vertex is associated with. This allows us to sequentially traverse the list
		// when applying the morph blends to each vertex.

		struct FCompareMorphTargetDeltas
		{
			FORCEINLINE bool operator()(const FMorphTargetDelta& A, const FMorphTargetDelta& B) const
			{
				return ((int32)A.SourceIdx - (int32)B.SourceIdx) < 0 ? true : false;
			}
		};

		MorphModel.Vertices.Sort(FCompareMorphTargetDeltas());

		// remove array slack
		MorphModel.Vertices.Shrink();
#if	UE_VERSION_OLDER_THAN(5,0,0)
#else
		MorphModel.NumVertices = MorphModel.Vertices.Num();
#endif
}
#endif

}

static bool readMorph2(TArray<FMorphTargetDelta> &MorphDeltas, aiString targetName,const aiScene *aiData, const UVrmAssetListObject *assetList) {

	MorphDeltas.Reset(0);
	uint32_t currentVertex = 0;

	FMorphTargetDelta morphinit;
#if	UE_VERSION_OLDER_THAN(5,0,0)
	morphinit.PositionDelta = FVector::ZeroVector;
	morphinit.SourceIdx = 0;
	morphinit.TangentZDelta = FVector::ZeroVector;
#else
	morphinit.PositionDelta = FVector3f::ZeroVector;
	morphinit.SourceIdx = 0;
	morphinit.TangentZDelta = FVector3f::ZeroVector;
#endif

	for (uint32_t m = 0; m < aiData->mNumMeshes; ++m) {
		const auto &mesh = assetList->MeshReturnedData->meshInfo[m];

		const aiMesh &aiM = *(aiData->mMeshes[m]);

		for (uint32_t a = 0; a < aiM.mNumAnimMeshes; ++a) {
			const aiAnimMesh &aiA = *(aiM.mAnimMeshes[a]);
			if (targetName != aiA.mName) {
				continue;
			}

			if (aiM.mNumVertices != aiA.mNumVertices) {
				UE_LOG(LogVRM4ULoader, Warning, TEXT("test18.\n"));
			}

			TArray<FMorphTargetDelta> tmpData;
			tmpData.Reserve(aiA.mNumVertices);

			bool bIncludeNormal = VRMConverter::Options::Get().IsEnableMorphTargetNormal();

			int VertexCount = 0;

			for (uint32_t i = 0; i < aiA.mNumVertices; ++i) {

				if (mesh.vertexUseFlag.Num() > 0) {
					if (mesh.vertexUseFlag[i] == false) {
						continue;
					}
				}

#if	UE_VERSION_OLDER_THAN(5,0,0)
				FMorphTargetDelta v = { FVector::ZeroVector, FVector::ZeroVector, 0 };
#else
				FMorphTargetDelta v = { FVector3f::ZeroVector, FVector3f::ZeroVector, 0 };
#endif
				v.SourceIdx = VertexCount + currentVertex;
				++VertexCount;

				if (aiA.mVertices) {
					v.PositionDelta.Set(
						-aiA.mVertices[i][0] * 100.f,
						aiA.mVertices[i][2] * 100.f,
						aiA.mVertices[i][1] * 100.f
					);
				}

				v.PositionDelta *= VRMConverter::Options::Get().GetModelScale();

				if (VRMConverter::Options::Get().IsVRM10Model()) {
					v.PositionDelta.X *= -1.f;
					v.PositionDelta.Y *= -1.f;
				}

				if (bIncludeNormal) {
#if	UE_VERSION_OLDER_THAN(5,0,0)
					const FVector n(
#else
					const FVector3f n(
#endif
						-aiA.mNormals[i][0],
						aiA.mNormals[i][2],
						aiA.mNormals[i][1]);
					if (n.Size() > 1.f) {
						v.TangentZDelta = n.GetUnsafeNormal();
					}

					if (VRMConverter::Options::Get().IsVRM10Model()) {
						v.TangentZDelta.X *= -1.f;
						v.TangentZDelta.Y *= -1.f;
					}
				}

				// skip invalid vertex data
				if (v.PositionDelta.Size() == 0) {
					if (bIncludeNormal == false) {
						continue;
					}
					if (v.TangentZDelta.Size() == 0) {
						continue;
					}
				}

				tmpData.Add(v);
			} // vertex loop


			MorphDeltas.Append(tmpData);
		}
		if (mesh.vertexUseFlag.Num() > 0) {
			currentVertex += mesh.useVertexCount;
		}else{
			currentVertex += aiM.mNumVertices;
		}
	}
	return MorphDeltas.Num() != 0;
}


bool VRMConverter::ConvertMorphTarget(UVrmAssetListObject *vrmAssetList) {
	if (Options::Get().IsSkipMorphTarget()) {
		return true;
	}

	if (vrmAssetList->SkeletalMesh == nullptr) {
		return false;
	}

	USkeletalMesh *sk = vrmAssetList->SkeletalMesh;

	TArray<FString> MorphNameList;
	TArray<FString> MorphNameList_Strict;
	TArray<UMorphTarget*> MorphTargetList;

	for (uint32_t m = 0; m < aiData->mNumMeshes; ++m) {
		const aiMesh &aiM = *(aiData->mMeshes[m]);
		for (uint32_t a = 0; a < aiM.mNumAnimMeshes; ++a) {
			const aiAnimMesh &aiA = *(aiM.mAnimMeshes[a]);
			//aiA.
			TArray<FMorphTargetDelta> MorphDeltas;

			FString morphName = UTF8_TO_TCHAR(aiA.mName.C_Str());
			FString morphNameOrg = morphName;
			if (VRMConverter::Options::Get().IsForceOriginalMorphTargetName()) {
			}else{
				morphName = VRMUtil::MakeName(morphName);
				//morphName = VRMConverter::NormalizeFileName(morphName);

				if (morphName != morphNameOrg) {
					if (MorphNameList_Strict.Find(morphNameOrg) == INDEX_NONE) {

						auto tmp = morphName;
						int i = 0;
						if (VRMUtil::IsNoSafeName(morphName)) {
							tmp = TEXT("UE5EA_patch_") + morphName + TEXT("_") + FString::FromInt(i);
						}
						while (MorphNameList.Find(tmp) != INDEX_NONE) {
							++i;
							tmp = TEXT("UE5EA_patch_") + morphName + TEXT("_") + FString::FromInt(i);
						}
						morphName = tmp;
					}
				}
			}
			if (morphName == TEXT("")) {
				//morphName = FString::Printf("%d_%d", m, a);
			}


			if (MorphNameList.Find(morphName) != INDEX_NONE) {
				continue;
			}

			MorphNameList.Add(morphName);
			MorphNameList_Strict.Add(morphNameOrg);
			if (readMorph2(MorphDeltas, aiA.mName, aiData, vrmAssetList) == false) {
				continue;
			}

			//FString sss = FString::Printf(TEXT("%02d_%02d_"), m, a) + FString(aiA.mName.C_Str());
			FString sss = morphName;// FString::Printf(TEXT("%02d_%02d_"), m, a) + FString();
			UMorphTarget *mt = NewObject<UMorphTarget>(sk, *sss);

#if WITH_EDITOR
			LocalPopulateDeltas(mt, MorphDeltas, 0, sk->GetImportedModel()->LODModels[0].Sections);
#else
			LocalPopulateDeltas(sk, mt, MorphDeltas, 0);
#endif

			if (mt->HasValidData()) {
#if	UE_VERSION_OLDER_THAN(4,20,0)
#else
				{
					FMorphTargetLODModel MorphLODModel;
					MorphLODModel.Reset();
					MorphLODModel.NumBaseMeshVerts = MorphDeltas.Num();
					MorphLODModel.SectionIndices.Add(0);
					MorphLODModel.Vertices = MorphDeltas;

#if	UE_VERSION_OLDER_THAN(5,0,0)
					mt->MorphLODModels.Add(MorphLODModel);
#else
					mt->GetMorphLODModels().Add(MorphLODModel);
#endif

					mt->BaseSkelMesh = sk;
				}
#endif
				MorphTargetList.Add(mt);
			}
		}
	}

#if WITH_EDITOR

#if	UE_VERSION_OLDER_THAN(4,25,0)
#else
	// to avoid no morph target
	// on Immediate
	VRMSetUseLegacyMeshDerivedDataKey(sk, true);

	FSkeletalMeshImportData RawMesh;
	sk->LoadLODImportedData(0, RawMesh);

	RawMesh.MorphTargetNames = MorphNameList;

	// to avoid no morph target
	// on EditorRestart
	sk->SaveLODImportedData(0, RawMesh);

	sk->SetLODImportedDataVersions(0, ESkeletalMeshGeoImportVersions::Before_Versionning, ESkeletalMeshSkinningImportVersions::Before_Versionning);

#endif
#endif // editor
#if	UE_VERSION_OLDER_THAN(5,0,0)
#else
	{
		FSkeletalMeshLODInfo* LODInfoPtr = sk->GetLODInfo(0);
		if (LODInfoPtr) {
			LODInfoPtr->ReductionSettings.NumOfTrianglesPercentage = 1.f;
		}
	}
#endif

	{
		// remove all morph & morph curve
		sk->UnregisterAllMorphTarget();

#if WITH_EDITOR
#if	UE_VERSION_OLDER_THAN(5,3,0)
#else
		{
			TArray<FName> namelist;
			VRMGetSkeleton(sk)->GetCurveMetaDataNames(namelist);
			for (auto name : namelist) {
				FCurveMetaData* m = VRMGetSkeleton(sk)->GetCurveMetaData(name);
				if (m == nullptr) continue;
				if (m->Type.bMorphtarget) {
					VRMGetSkeleton(sk)->RemoveCurveMetaData(name);
				}
			}
		}
#endif
#endif
	}

	for (int i=0; i<MorphTargetList.Num(); ++i){
		auto *mt = MorphTargetList[i];
		if (i == MorphTargetList.Num() - 1) {
			sk->RegisterMorphTarget(mt);
		} else {
			VRMGetMorphTargets(sk).Add(mt);
		}
	}

	for (auto name : MorphNameList) {
		FCurveMetaData* FoundCurveMetaData = VRMGetSkeleton(sk)->GetCurveMetaData(*name);
		if (FoundCurveMetaData) {
			FoundCurveMetaData->Type.bMorphtarget = true;
			continue;
		}

		VRMGetSkeleton(sk)->AccumulateCurveMetaData(*name, false, true);
	}

#if WITH_EDITOR
	sk->PostEditChange();
#else

#if	UE_VERSION_OLDER_THAN(4,24,0)
#else
	if (sk->GetResourceForRendering()) {
		if (sk->GetResourceForRendering()->LODRenderData.Num() > 0) {

#if	UE_VERSION_OLDER_THAN(5,0,0)
#else
			if (VRMConverter::IsImportMode() == false) {
				for (auto& RenderSection : sk->GetResourceForRendering()->LODRenderData[0].RenderSections) {
					RenderSection.DuplicatedVerticesBuffer.DupVertData.SetNum(1);
				}
			}
#endif
			sk->GetResourceForRendering()->LODRenderData[0].InitResources(false, 0, VRMGetMorphTargets(sk), sk);
		}
	}
#endif

#endif // editor

	return true;
}


VrmConvertMorphTarget::VrmConvertMorphTarget()
{
}

VrmConvertMorphTarget::~VrmConvertMorphTarget()
{
}
