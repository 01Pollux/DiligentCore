/*
 *  Copyright 2019-2022 Diligent Graphics LLC
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *  In no event and under no legal theory, whether in tort (including negligence),
 *  contract, or otherwise, unless required by applicable law (such as deliberate
 *  and grossly negligent acts) or agreed to in writing, shall any Contributor be
 *  liable for any damages, including any direct, indirect, special, incidental,
 *  or consequential damages of any character arising as a result of this License or
 *  out of the use or inability to use the software (including but not limited to damages
 *  for loss of goodwill, work stoppage, computer failure or malfunction, or any and
 *  all other commercial damages or losses), even if such Contributor has been advised
 *  of the possibility of such damages.
 */

#include "ArchiverImpl.hpp"
#include "Archiver_Inc.hpp"

#include <thread>
#include <sstream>
#include <filesystem>
#include <vector>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include "RenderDeviceMtlImpl.hpp"
#include "PipelineResourceSignatureMtlImpl.hpp"
#include "PipelineStateMtlImpl.hpp"
#include "ShaderMtlImpl.hpp"
#include "DeviceObjectArchiveMtlImpl.hpp"
#include "SerializedPipelineStateImpl.hpp"
#include "FileSystem.hpp"

#include "spirv_msl.hpp"

namespace filesystem = std::__fs::filesystem;

namespace Diligent
{

template <>
struct SerializedResourceSignatureImpl::SignatureTraits<PipelineResourceSignatureMtlImpl>
{
    static constexpr DeviceType Type = DeviceType::Metal_MacOS;

    template <SerializerMode Mode>
    using PRSSerializerType = PRSSerializerMtl<Mode>;
};

namespace
{

std::string GetTmpFolder()
{
    const auto ProcId   = getpid();
    const auto ThreadId = std::this_thread::get_id();
    std::stringstream FolderPath;
    FolderPath << filesystem::temp_directory_path().c_str()
               << "/DiligentArchiver-"
               << ProcId << '-'
               << ThreadId << '/';
    return FolderPath.str();
}

struct ShaderStageInfoMtl
{
    ShaderStageInfoMtl() {}

    ShaderStageInfoMtl(const SerializedShaderImpl* _pShader) :
        Type{_pShader->GetDesc().ShaderType},
        pShader{_pShader}
    {}

    // Needed only for ray tracing
    void Append(const SerializedShaderImpl*) {}

    constexpr Uint32 Count() const { return 1; }

    SHADER_TYPE                 Type    = SHADER_TYPE_UNKNOWN;
    const SerializedShaderImpl* pShader = nullptr;
};

#ifdef DILIGENT_DEBUG
inline SHADER_TYPE GetShaderStageType(const ShaderStageInfoMtl& Stage)
{
    return Stage.Type;
}
#endif

} // namespace


template <typename CreateInfoType>
void SerializedPipelineStateImpl::PatchShadersMtl(const CreateInfoType& CreateInfo, DeviceType DevType, const std::string& DumpDir) noexcept(false)
{
    VERIFY_EXPR(DevType == DeviceType::Metal_MacOS || DevType == DeviceType::Metal_iOS);

    std::vector<ShaderStageInfoMtl> ShaderStages;
    SHADER_TYPE                     ActiveShaderStages = SHADER_TYPE_UNKNOWN;
    PipelineStateMtlImpl::ExtractShaders<SerializedShaderImpl>(CreateInfo, ShaderStages, ActiveShaderStages);

    std::vector<const MSLParseData*> StageResources{ShaderStages.size()};
    for (size_t i = 0; i < StageResources.size(); ++i)
    {
        StageResources[i] = ShaderStages[i].pShader->GetMSLData();
    }

    auto** ppSignatures    = CreateInfo.ppResourceSignatures;
    auto   SignaturesCount = CreateInfo.ResourceSignaturesCount;

    IPipelineResourceSignature* DefaultSignatures[1] = {};
    if (CreateInfo.ResourceSignaturesCount == 0)
    {
        CreateDefaultResourceSignature<PipelineStateMtlImpl, PipelineResourceSignatureMtlImpl>(DevType, CreateInfo.PSODesc, ActiveShaderStages, StageResources);

        DefaultSignatures[0] = m_pDefaultSignature;
        SignaturesCount      = 1;
        ppSignatures         = DefaultSignatures;
    }

    {
        // Sort signatures by binding index.
        // Note that SignaturesCount will be overwritten with the maximum binding index.
        SignatureArray<PipelineResourceSignatureMtlImpl> Signatures      = {};
        SortResourceSignatures(ppSignatures, SignaturesCount, Signatures, SignaturesCount, DevType);

        std::array<MtlResourceCounters, MAX_RESOURCE_SIGNATURES> BaseBindings{};
        MtlResourceCounters                                      CurrBindings{};
        for (Uint32 s = 0; s < SignaturesCount; ++s)
        {
            BaseBindings[s] = CurrBindings;
            const auto& pSignature = Signatures[s];
            if (pSignature != nullptr)
                pSignature->ShiftBindings(CurrBindings);
        }

        VERIFY_EXPR(m_Data.Shaders[static_cast<size_t>(DevType)].empty());
        for (size_t j = 0; j < ShaderStages.size(); ++j)
        {
            const auto& Stage = ShaderStages[j];
            // Note that patched shader data contains some extra information
            // besides the byte code itself.
            const auto ShaderData = Stage.pShader->PatchShaderMtl(
                CreateInfo.PSODesc.Name, DumpDir, Signatures.data(),
                BaseBindings.data(), SignaturesCount, DevType); // May throw

            auto ShaderCI           = Stage.pShader->GetCreateInfo();
            ShaderCI.Source         = nullptr;
            ShaderCI.FilePath       = nullptr;
            ShaderCI.SourceLanguage = SHADER_SOURCE_LANGUAGE_MTLB;
            ShaderCI.ByteCode       = ShaderData.Ptr();
            ShaderCI.ByteCodeSize   = ShaderData.Size();
            SerializeShaderCreateInfo(DevType, ShaderCI);
        }
        VERIFY_EXPR(m_Data.Shaders[static_cast<size_t>(DevType)].size() == ShaderStages.size());
    }
}

INSTANTIATE_PATCH_SHADER_METHODS(PatchShadersMtl, DeviceType DevType, const std::string& DumpDir)
INSTANTIATE_DEVICE_SIGNATURE_METHODS(PipelineResourceSignatureMtlImpl)


static_assert(std::is_same<MtlArchiverResourceCounters, MtlResourceCounters>::value,
              "MtlArchiverResourceCounters and MtlResourceCounters must be same types");

struct SerializedShaderImpl::CompiledShaderMtlImpl final : CompiledShader
{
    MSLParseData MSLData;
};

void SerializedShaderImpl::CreateShaderMtl(ShaderCreateInfo ShaderCI) noexcept(false)
{
    auto pShaderMtl = std::make_unique<CompiledShaderMtlImpl>();
    // Convert HLSL/GLSL/SPIRV to MSL
    pShaderMtl->MSLData = ShaderMtlImpl::PrepareMSLData(
        ShaderCI,
        m_pDevice->GetDeviceInfo(),
        m_pDevice->GetAdapterInfo()); // may throw exception
    m_pShaderMtl = std::move(pShaderMtl);
}

const MSLParseData* SerializedShaderImpl::GetMSLData() const
{
    auto* pShaderMtl = static_cast<const CompiledShaderMtlImpl*>(m_pShaderMtl.get());
    return pShaderMtl != nullptr ? &pShaderMtl->MSLData : nullptr;
}

namespace
{

template <SerializerMode Mode>
void SerializeMSLData(Serializer<Mode>&   Ser,
                      const SHADER_TYPE   ShaderType,
                      const MSLParseData& MSLData)
{
    // Same as DeviceObjectArchiveMtlImpl::UnpackShader
    
    const auto& BufferInfoMap{MSLData.BufferInfoMap};
    const auto Count = static_cast<Uint32>(BufferInfoMap.size());
    Ser(Count);
    for (auto& it : BufferInfoMap)
    {
        const auto* Name    = it.first.c_str();
        const auto* AltName = it.second.AltName.c_str();
        const auto  Space   = it.second.Space;
        Ser(Name, AltName, Space);
    }

    if (ShaderType == SHADER_TYPE_COMPUTE)
    {
        Ser(MSLData.ComputeGroupSize);
    }
}

} // namespace

SerializedData SerializedShaderImpl::PatchShaderMtl(const char*                                            PSOName,
                                                    const std::string&                                     DumpFolder,
                                                    const RefCntAutoPtr<PipelineResourceSignatureMtlImpl>* pSignatures,
                                                    const MtlResourceCounters*                             pBaseBindings,
                                                    const Uint32                                           SignatureCount,
                                                    DeviceType                                             DevType) const noexcept(false)
{
    VERIFY_EXPR(SignatureCount > 0);
    VERIFY_EXPR(pSignatures != nullptr);
    VERIFY_EXPR(pBaseBindings != nullptr);
    VERIFY_EXPR(PSOName != nullptr && PSOName[0] != '\0');

    const auto* ShaderName = GetDesc().Name;
    VERIFY_EXPR(ShaderName != nullptr);

    const std::string WorkingFolder =
        [&](){
            if (DumpFolder.empty())
                return GetTmpFolder();

            auto Folder = DumpFolder;
            if (Folder.back() != FileSystem::GetSlashSymbol())
                Folder += FileSystem::GetSlashSymbol();

            return Folder;
        }();
    filesystem::create_directories(WorkingFolder);

    struct TmpDirRemover
    {
        explicit TmpDirRemover(const std::string& _Path) noexcept :
            Path{_Path}
        {}

        ~TmpDirRemover()
        {
            if (!Path.empty())
            {
                filesystem::remove_all(Path.c_str());
            }
        }
    private:
        const std::string Path;
    };
    TmpDirRemover DirRemover{DumpFolder.empty() ? WorkingFolder : ""};

    const auto MetalFile    = WorkingFolder + ShaderName + ".metal";
    const auto MetalLibFile = WorkingFolder + ShaderName + ".metallib";

    auto*  pShaderMtl = static_cast<const CompiledShaderMtlImpl*>(m_pShaderMtl.get());
    String MslSource = pShaderMtl->MSLData.Source;
    if (pShaderMtl->MSLData.pParser)
    {
        const auto ResRemapping = PipelineStateMtlImpl::GetResourceMap(
            pShaderMtl->MSLData,
            pSignatures,
            SignatureCount,
            pBaseBindings,
            GetDesc(),
            ""); // may throw exception

        MslSource = pShaderMtl->MSLData.pParser->RemapResources(ResRemapping);
        if (MslSource.empty())
            LOG_ERROR_AND_THROW("Failed to remap MSL resources");
    }

#define LOG_PATCH_SHADER_ERROR_AND_THROW(...)\
    LOG_ERROR_AND_THROW("Failed to patch shader '", ShaderName, "' for PSO '", PSOName, "': ", ##__VA_ARGS__)

#define LOG_ERRNO_AND_THROW(...)                      \
    do                                                \
    {                                                 \
        char ErrorStr[512];                           \
        strerror_r(errno, ErrorStr, sizeof(ErrorStr));\
        LOG_PATCH_SHADER_ERROR_AND_THROW(" Error description: ", ErrorStr);\
    } while (false)

    // Save to 'Shader.metal'
    {
        FILE* File = fopen(MetalFile.c_str(), "wb");
        if (File == nullptr)
            LOG_ERRNO_AND_THROW("failed to open temp file to save Metal shader source.");

        if (fwrite(MslSource.c_str(), sizeof(MslSource[0]) * MslSource.size(), 1, File) != 1)
            LOG_ERRNO_AND_THROW("failed to save Metal shader source to a temp file.");

        fclose(File);
    }

    const auto& MtlProps = m_pDevice->GetMtlProperties();
    // Run user-defined MSL preprocessor
    if (!MtlProps.MslPreprocessorCmd.empty())
    {
        auto cmd{MtlProps.MslPreprocessorCmd};
        cmd += " \"";
        cmd += MetalFile;
        cmd += '\"';
        FILE* Pipe = FileSystem::popen(cmd.c_str(), "r");
        if (Pipe == nullptr)
            LOG_ERRNO_AND_THROW("failed to run command-line Metal shader compiler.");

        char Output[512];
        while (fgets(Output, _countof(Output), Pipe) != nullptr)
            printf("%s", Output);

        auto status = FileSystem::pclose(Pipe);
        if (status != 0)
            LOG_PATCH_SHADER_ERROR_AND_THROW("failed to close msl preprocessor process (error code: ", status, ").");
    }

    // https://developer.apple.com/documentation/metal/libraries/generating_and_loading_a_metal_library_symbol_file

    // Compile MSL to Metal library
    {
        String cmd{"xcrun "};
        cmd += (DevType == DeviceType::Metal_MacOS ? MtlProps.CompileOptionsMacOS : MtlProps.CompileOptionsIOS);
        cmd += " \"" + MetalFile + "\" -o \"" + MetalLibFile + '\"';

        FILE* Pipe = FileSystem::popen(cmd.c_str(), "r");
        if (Pipe == nullptr)
            LOG_ERRNO_AND_THROW("failed to compile MSL source.");

        char Output[512];
        while (fgets(Output, _countof(Output), Pipe) != nullptr)
            printf("%s", Output);

        auto status = FileSystem::pclose(Pipe);
        if (status != 0)
            LOG_PATCH_SHADER_ERROR_AND_THROW("failed to close xcrun process (error code: ", status, ").");
    }

    // Read 'Shader.metallib'
    std::vector<Uint8> ByteCode;
    {
        FILE* File = fopen(MetalLibFile.c_str(), "rb");
        if (File == nullptr)
            LOG_ERRNO_AND_THROW("failed to read shader library.");

        fseek(File, 0, SEEK_END);
        const auto BytecodeSize = static_cast<size_t>(ftell(File));
        fseek(File, 0, SEEK_SET);
        ByteCode.resize(BytecodeSize);
        if (fread(ByteCode.data(), BytecodeSize, 1, File) != 1)
            ByteCode.clear();

        fclose(File);
    }

    if (ByteCode.empty())
        LOG_PATCH_SHADER_ERROR_AND_THROW("Metal shader library is empty.");

#undef LOG_PATCH_SHADER_ERROR_AND_THROW
#undef LOG_ERRNO_AND_THROW

    auto SerializeShaderData = [&](auto& Ser){
        Ser.SerializeBytes(ByteCode.data(), ByteCode.size() * sizeof(ByteCode[0]));
        SerializeMSLData(Ser, GetDesc().ShaderType, pShaderMtl->MSLData);
    };

    SerializedData ShaderData;
    {
        Serializer<SerializerMode::Measure> Ser;
        SerializeShaderData(Ser);
        ShaderData = Ser.AllocateData(GetRawAllocator());
    }

    {
        Serializer<SerializerMode::Write> Ser{ShaderData};
        SerializeShaderData(Ser);
        VERIFY_EXPR(Ser.IsEnded());
    }

    return ShaderData;
}

void SerializationDeviceImpl::GetPipelineResourceBindingsMtl(const PipelineResourceBindingAttribs& Info,
                                                             std::vector<PipelineResourceBinding>& ResourceBindings,
                                                             const Uint32                          MaxBufferArgs)
{
    ResourceBindings.clear();

    std::array<RefCntAutoPtr<PipelineResourceSignatureMtlImpl>, MAX_RESOURCE_SIGNATURES> Signatures = {};

    Uint32 SignaturesCount = 0;
    for (Uint32 i = 0; i < Info.ResourceSignaturesCount; ++i)
    {
        const auto* pSerPRS = ClassPtrCast<SerializedResourceSignatureImpl>(Info.ppResourceSignatures[i]);
        const auto& Desc    = pSerPRS->GetDesc();

        Signatures[Desc.BindingIndex] = pSerPRS->GetDeviceSignature<PipelineResourceSignatureMtlImpl>(DeviceObjectArchiveBase::DeviceType::Metal_MacOS);
        SignaturesCount               = std::max(SignaturesCount, static_cast<Uint32>(Desc.BindingIndex) + 1);
    }

    const auto          ShaderStages        = (Info.ShaderStages == SHADER_TYPE_UNKNOWN ? static_cast<SHADER_TYPE>(~0u) : Info.ShaderStages);
    constexpr auto      SupportedStagesMask = (SHADER_TYPE_VERTEX | SHADER_TYPE_PIXEL | SHADER_TYPE_COMPUTE | SHADER_TYPE_TILE);
    MtlResourceCounters BaseBindings{};

    for (Uint32 sign = 0; sign < SignaturesCount; ++sign)
    {
        const auto& pSignature = Signatures[sign];
        if (pSignature == nullptr)
            continue;

        for (Uint32 r = 0; r < pSignature->GetTotalResourceCount(); ++r)
        {
            const auto& ResDesc = pSignature->GetResourceDesc(r);
            const auto& ResAttr = pSignature->GetResourceAttribs(r);
            const auto  Range   = PipelineResourceDescToMtlResourceRange(ResDesc);

            for (auto Stages = ShaderStages & SupportedStagesMask; Stages != 0;)
            {
                const auto ShaderStage = ExtractLSB(Stages);
                const auto ShaderInd   = MtlResourceBindIndices::ShaderTypeToIndex(ShaderStage);
                DEV_CHECK_ERR(ShaderInd < MtlResourceBindIndices::NumShaderTypes,
                              "Unsupported shader stage (", GetShaderTypeLiteralName(ShaderStage), ") for Metal backend");

                if ((ResDesc.ShaderStages & ShaderStage) == 0)
                    continue;

                ResourceBindings.push_back(ResDescToPipelineResBinding(ResDesc, ShaderStage, BaseBindings[ShaderInd][Range] + ResAttr.BindIndices[ShaderInd], 0 /*space*/));
            }
        }
        pSignature->ShiftBindings(BaseBindings);
    }

    // Add vertex buffer bindings.
    // Same as DeviceContextMtlImpl::CommitVertexBuffers()
    if ((Info.ShaderStages & SHADER_TYPE_VERTEX) != 0 && Info.NumVertexBuffers > 0)
    {
        DEV_CHECK_ERR(Info.VertexBufferNames != nullptr, "VertexBufferNames must not be null");

        const auto BaseSlot = MaxBufferArgs - Info.NumVertexBuffers;
        for (Uint32 i = 0; i < Info.NumVertexBuffers; ++i)
        {
            PipelineResourceBinding Dst{};
            Dst.Name         = Info.VertexBufferNames[i];
            Dst.ResourceType = SHADER_RESOURCE_TYPE_BUFFER_SRV;
            Dst.Register     = BaseSlot + i;
            Dst.Space        = 0;
            Dst.ArraySize    = 1;
            Dst.ShaderStages = SHADER_TYPE_VERTEX;
            ResourceBindings.push_back(Dst);
        }
    }
}

} // namespace Diligent
