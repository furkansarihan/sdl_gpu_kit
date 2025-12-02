// dds_loader.h
#pragma once

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_log.h>
#include <cstring>

// DDS Format Constants
const Uint32 DDS_MAGIC = 0x20534444; // "DDS "

// DDS_PIXELFORMAT flags
const Uint32 DDPF_ALPHAPIXELS = 0x00000001;
const Uint32 DDPF_ALPHA = 0x00000002;
const Uint32 DDPF_FOURCC = 0x00000004;
const Uint32 DDPF_RGB = 0x00000040;
const Uint32 DDPF_YUV = 0x00000200;
const Uint32 DDPF_LUMINANCE = 0x00020000;

// DDS_HEADER flags
const Uint32 DDSD_CAPS = 0x00000001;
const Uint32 DDSD_HEIGHT = 0x00000002;
const Uint32 DDSD_WIDTH = 0x00000004;
const Uint32 DDSD_PITCH = 0x00000008;
const Uint32 DDSD_PIXELFORMAT = 0x00001000;
const Uint32 DDSD_MIPMAPCOUNT = 0x00020000;
const Uint32 DDSD_LINEARSIZE = 0x00080000;
const Uint32 DDSD_DEPTH = 0x00800000;

// FourCC codes
const Uint32 FOURCC_DXT1 = 0x31545844; // "DXT1"
const Uint32 FOURCC_DXT3 = 0x33545844; // "DXT3"
const Uint32 FOURCC_DXT5 = 0x35545844; // "DXT5"
const Uint32 FOURCC_BC4U = 0x55344342; // "BC4U"
const Uint32 FOURCC_BC4S = 0x53344342; // "BC4S"
const Uint32 FOURCC_BC5U = 0x55354342; // "BC5U"
const Uint32 FOURCC_BC5S = 0x53354342; // "BC5S"
const Uint32 FOURCC_ATI1 = 0x31495441; // "ATI1" (BC4)
const Uint32 FOURCC_ATI2 = 0x32495441; // "ATI2" (BC5)
const Uint32 FOURCC_DX10 = 0x30315844; // "DX10"

struct DDS_PIXELFORMAT
{
    Uint32 dwSize;
    Uint32 dwFlags;
    Uint32 dwFourCC;
    Uint32 dwRGBBitCount;
    Uint32 dwRBitMask;
    Uint32 dwGBitMask;
    Uint32 dwBBitMask;
    Uint32 dwABitMask;
};

struct DDS_HEADER
{
    Uint32 dwSize;
    Uint32 dwFlags;
    Uint32 dwHeight;
    Uint32 dwWidth;
    Uint32 dwPitchOrLinearSize;
    Uint32 dwDepth;
    Uint32 dwMipMapCount;
    Uint32 dwReserved1[11];
    DDS_PIXELFORMAT ddspf;
    Uint32 dwCaps;
    Uint32 dwCaps2;
    Uint32 dwCaps3;
    Uint32 dwCaps4;
    Uint32 dwReserved2;
};

struct DDS_HEADER_DXT10
{
    Uint32 dxgiFormat;
    Uint32 resourceDimension;
    Uint32 miscFlag;
    Uint32 arraySize;
    Uint32 miscFlags2;
};

struct DDSTextureInfo
{
    SDL_GPUTexture *texture;
    Uint32 width;
    Uint32 height;
    Uint32 mipLevels;
    SDL_GPUTextureFormat format;
    bool isCompressed;
};

class DDSLoader
{
public:
    static DDSTextureInfo *LoadFromFile(SDL_GPUDevice *device, const char *filepath)
    {
        size_t dataSize;
        void *fileData = SDL_LoadFile(filepath, &dataSize);

        if (!fileData)
        {
            SDL_Log("Failed to load DDS file: %s - %s", filepath, SDL_GetError());
            return nullptr;
        }

        DDSTextureInfo *result = LoadFromMemory(device, fileData, dataSize);
        SDL_free(fileData);

        if (result)
        {
            SDL_Log("Successfully loaded DDS: %s (%dx%d, %d mips)",
                    filepath, result->width, result->height, result->mipLevels);
        }

        return result;
    }

    static DDSTextureInfo *LoadFromMemory(SDL_GPUDevice *device, void *data, size_t dataSize)
    {
        if (!data || dataSize < sizeof(Uint32) + sizeof(DDS_HEADER))
        {
            SDL_Log("DDS: Invalid data size");
            return nullptr;
        }

        const Uint8 *ptr = static_cast<const Uint8 *>(data);

        // Verify magic number
        Uint32 magic = *reinterpret_cast<const Uint32 *>(ptr);
        if (magic != DDS_MAGIC)
        {
            SDL_Log("DDS: Invalid magic number: 0x%08X (expected 0x%08X)", magic, DDS_MAGIC);
            return nullptr;
        }
        ptr += sizeof(Uint32);

        // Read header
        const DDS_HEADER *header = reinterpret_cast<const DDS_HEADER *>(ptr);
        ptr += sizeof(DDS_HEADER);

        // Validate header
        if (header->dwSize != 124)
        {
            SDL_Log("DDS: Invalid header size: %u", header->dwSize);
            return nullptr;
        }

        if (!(header->dwFlags & DDSD_WIDTH) || !(header->dwFlags & DDSD_HEIGHT))
        {
            SDL_Log("DDS: Missing width/height flags");
            return nullptr;
        }

        Uint32 width = header->dwWidth;
        Uint32 height = header->dwHeight;
        Uint32 mipLevels = (header->dwFlags & DDSD_MIPMAPCOUNT) ? header->dwMipMapCount : 1;

        // Check for DX10 extended header
        bool hasDX10Header = (header->ddspf.dwFlags & DDPF_FOURCC) &&
                             (header->ddspf.dwFourCC == FOURCC_DX10);

        const DDS_HEADER_DXT10 *dx10Header = nullptr;
        if (hasDX10Header)
        {
            dx10Header = reinterpret_cast<const DDS_HEADER_DXT10 *>(ptr);
            ptr += sizeof(DDS_HEADER_DXT10);
        }

        // Determine format
        SDL_GPUTextureFormat format;
        bool isCompressed;
        size_t blockSize;

        if (!DetermineFormat(header, dx10Header, format, isCompressed, blockSize))
        {
            SDL_Log("DDS: Unsupported or unknown format");
            return nullptr;
        }

        // Calculate expected data size
        size_t expectedSize = CalculateTextureSize(width, height, mipLevels,
                                                   isCompressed, blockSize);
        size_t headerSize = ptr - static_cast<const Uint8 *>(data);
        size_t remainingSize = dataSize - headerSize;

        if (remainingSize < expectedSize)
        {
            SDL_Log("DDS: Insufficient data (expected %zu, got %zu)",
                    expectedSize, remainingSize);
            return nullptr;
        }

        // Create texture
        SDL_GPUTextureCreateInfo texInfo{};
        texInfo.type = SDL_GPU_TEXTURETYPE_2D;
        texInfo.format = format;
        texInfo.width = width;
        texInfo.height = height;
        texInfo.layer_count_or_depth = 1;
        texInfo.num_levels = mipLevels;
        texInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;

        SDL_GPUTexture *texture = SDL_CreateGPUTexture(device, &texInfo);
        if (!texture)
        {
            SDL_Log("DDS: Failed to create GPU texture: %s", SDL_GetError());
            return nullptr;
        }

        // Upload texture data
        if (!UploadTextureData(device, texture, ptr, width, height,
                               mipLevels, isCompressed, blockSize))
        {
            SDL_ReleaseGPUTexture(device, texture);
            return nullptr;
        }

        // Create result
        DDSTextureInfo *info = new DDSTextureInfo();
        info->texture = texture;
        info->width = width;
        info->height = height;
        info->mipLevels = mipLevels;
        info->format = format;
        info->isCompressed = isCompressed;

        return info;
    }

    static void Release(SDL_GPUDevice *device, DDSTextureInfo *info)
    {
        if (info)
        {
            if (info->texture)
            {
                SDL_ReleaseGPUTexture(device, info->texture);
            }
            delete info;
        }
    }

private:
    static bool DetermineFormat(const DDS_HEADER *header,
                                const DDS_HEADER_DXT10 *dx10Header,
                                SDL_GPUTextureFormat &format,
                                bool &isCompressed,
                                size_t &blockSize)
    {
        const DDS_PIXELFORMAT &pf = header->ddspf;

        // Handle DX10 format
        if (dx10Header)
        {
            return DetermineDX10Format(dx10Header->dxgiFormat, format,
                                       isCompressed, blockSize);
        }

        // Handle FourCC compressed formats
        if (pf.dwFlags & DDPF_FOURCC)
        {
            return DetermineFourCCFormat(pf.dwFourCC, format,
                                         isCompressed, blockSize);
        }

        // Handle uncompressed formats
        return DetermineUncompressedFormat(pf, format, isCompressed, blockSize);
    }

    static bool DetermineFourCCFormat(Uint32 fourCC,
                                      SDL_GPUTextureFormat &format,
                                      bool &isCompressed,
                                      size_t &blockSize)
    {
        isCompressed = true;

        switch (fourCC)
        {
        case FOURCC_DXT1:
            format = SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM;
            blockSize = 8;
            return true;

        case FOURCC_DXT3:
            format = SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM;
            blockSize = 16;
            return true;

        case FOURCC_DXT5:
            format = SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM;
            blockSize = 16;
            return true;

        case FOURCC_BC4U:
        case FOURCC_ATI1:
            format = SDL_GPU_TEXTUREFORMAT_BC4_R_UNORM;
            blockSize = 8;
            return true;

            // case FOURCC_BC4S:
            //     format = SDL_GPU_TEXTUREFORMAT_BC4_R_SNORM;
            //     blockSize = 8;
            //     return true;

        case FOURCC_BC5U:
        case FOURCC_ATI2:
            format = SDL_GPU_TEXTUREFORMAT_BC5_RG_UNORM;
            blockSize = 16;
            return true;

            // case FOURCC_BC5S:
            //     format = SDL_GPU_TEXTUREFORMAT_BC5_RG_SNORM;
            //     blockSize = 16;
            //     return true;

        default:
            SDL_Log("DDS: Unknown FourCC: 0x%08X", fourCC);
            return false;
        }
    }

    static bool DetermineUncompressedFormat(const DDS_PIXELFORMAT &pf,
                                            SDL_GPUTextureFormat &format,
                                            bool &isCompressed,
                                            size_t &blockSize)
    {
        isCompressed = false;
        blockSize = pf.dwRGBBitCount / 8;

        // R8
        if ((pf.dwFlags & DDPF_LUMINANCE) && pf.dwRGBBitCount == 8)
        {
            format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
            return true;
        }

        // R8G8 (Luminance-Alpha or explicit RG)
        if (pf.dwRGBBitCount == 16)
        {
            if ((pf.dwFlags & DDPF_LUMINANCE) ||
                (pf.dwRBitMask == 0x00FF && pf.dwGBitMask == 0xFF00))
            {
                format = SDL_GPU_TEXTUREFORMAT_R8G8_UNORM;
                return true;
            }
        }

        // R8G8B8A8
        if ((pf.dwFlags & DDPF_RGB) && pf.dwRGBBitCount == 32)
        {
            // RGBA8
            if (pf.dwRBitMask == 0x000000FF &&
                pf.dwGBitMask == 0x0000FF00 &&
                pf.dwBBitMask == 0x00FF0000 &&
                pf.dwABitMask == 0xFF000000)
            {
                format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
                return true;
            }

            // BGRA8
            if (pf.dwRBitMask == 0x00FF0000 &&
                pf.dwGBitMask == 0x0000FF00 &&
                pf.dwBBitMask == 0x000000FF &&
                pf.dwABitMask == 0xFF000000)
            {
                format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
                return true;
            }
        }

        // R8G8B8 (24-bit, less common)
        if ((pf.dwFlags & DDPF_RGB) && pf.dwRGBBitCount == 24)
        {
            SDL_Log("DDS: 24-bit RGB format not directly supported, "
                    "consider converting to RGBA8");
            return false;
        }

        SDL_Log("DDS: Unsupported uncompressed format (bits=%u, R=0x%08X, "
                "G=0x%08X, B=0x%08X, A=0x%08X)",
                pf.dwRGBBitCount, pf.dwRBitMask, pf.dwGBitMask,
                pf.dwBBitMask, pf.dwABitMask);
        return false;
    }

    static bool DetermineDX10Format(Uint32 dxgiFormat,
                                    SDL_GPUTextureFormat &format,
                                    bool &isCompressed,
                                    size_t &blockSize)
    {
        // DXGI_FORMAT enum values (subset)
        enum DXGI_FORMAT
        {
            DXGI_FORMAT_BC1_UNORM = 71,
            DXGI_FORMAT_BC2_UNORM = 74,
            DXGI_FORMAT_BC3_UNORM = 77,
            DXGI_FORMAT_BC4_UNORM = 80,
            DXGI_FORMAT_BC4_SNORM = 81,
            DXGI_FORMAT_BC5_UNORM = 83,
            DXGI_FORMAT_BC5_SNORM = 84,
            DXGI_FORMAT_BC6H_UF16 = 95,
            DXGI_FORMAT_BC6H_SF16 = 96,
            DXGI_FORMAT_BC7_UNORM = 98,
            DXGI_FORMAT_R8_UNORM = 61,
            DXGI_FORMAT_R8G8_UNORM = 49,
            DXGI_FORMAT_R8G8B8A8_UNORM = 28,
            DXGI_FORMAT_B8G8R8A8_UNORM = 87,
        };

        switch (dxgiFormat)
        {
        case DXGI_FORMAT_BC1_UNORM:
            format = SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM;
            isCompressed = true;
            blockSize = 8;
            return true;

        case DXGI_FORMAT_BC2_UNORM:
            format = SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM;
            isCompressed = true;
            blockSize = 16;
            return true;

        case DXGI_FORMAT_BC3_UNORM:
            format = SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM;
            isCompressed = true;
            blockSize = 16;
            return true;

        case DXGI_FORMAT_BC4_UNORM:
            format = SDL_GPU_TEXTUREFORMAT_BC4_R_UNORM;
            isCompressed = true;
            blockSize = 8;
            return true;

            // case DXGI_FORMAT_BC4_SNORM:
            //     format = SDL_GPU_TEXTUREFORMAT_BC4_R_SNORM;
            //     isCompressed = true;
            //     blockSize = 8;
            //     return true;

        case DXGI_FORMAT_BC5_UNORM:
            format = SDL_GPU_TEXTUREFORMAT_BC5_RG_UNORM;
            isCompressed = true;
            blockSize = 16;
            return true;

            // case DXGI_FORMAT_BC5_SNORM:
            //     format = SDL_GPU_TEXTUREFORMAT_BC5_RG_SNORM;
            //     isCompressed = true;
            //     blockSize = 16;
            //     return true;

        case DXGI_FORMAT_BC6H_UF16:
            format = SDL_GPU_TEXTUREFORMAT_BC6H_RGB_UFLOAT;
            isCompressed = true;
            blockSize = 16;
            return true;

            // case DXGI_FORMAT_BC6H_SF16:
            //     format = SDL_GPU_TEXTUREFORMAT_BC6H_RGB_SFLOAT;
            //     isCompressed = true;
            //     blockSize = 16;
            //     return true;

        case DXGI_FORMAT_BC7_UNORM:
            format = SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM;
            isCompressed = true;
            blockSize = 16;
            return true;

        case DXGI_FORMAT_R8_UNORM:
            format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
            isCompressed = false;
            blockSize = 1;
            return true;

        case DXGI_FORMAT_R8G8_UNORM:
            format = SDL_GPU_TEXTUREFORMAT_R8G8_UNORM;
            isCompressed = false;
            blockSize = 2;
            return true;

        case DXGI_FORMAT_R8G8B8A8_UNORM:
            format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
            isCompressed = false;
            blockSize = 4;
            return true;

        case DXGI_FORMAT_B8G8R8A8_UNORM:
            format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
            isCompressed = false;
            blockSize = 4;
            return true;

        default:
            SDL_Log("DDS: Unsupported DX10 format: %u", dxgiFormat);
            return false;
        }
    }

    static size_t CalculateTextureSize(Uint32 width, Uint32 height,
                                       Uint32 mipLevels, bool isCompressed,
                                       size_t blockSize)
    {
        size_t totalSize = 0;

        for (Uint32 mip = 0; mip < mipLevels; ++mip)
        {
            Uint32 mipWidth = width >> mip;
            Uint32 mipHeight = height >> mip;
            if (mipWidth == 0)
                mipWidth = 1;
            if (mipHeight == 0)
                mipHeight = 1;

            size_t mipSize;
            if (isCompressed)
            {
                // Compressed formats use 4x4 blocks
                Uint32 blocksWide = (mipWidth + 3) / 4;
                Uint32 blocksHigh = (mipHeight + 3) / 4;
                mipSize = blocksWide * blocksHigh * blockSize;
            }
            else
            {
                mipSize = mipWidth * mipHeight * blockSize;
            }

            totalSize += mipSize;
        }

        return totalSize;
    }

    static bool UploadTextureData(SDL_GPUDevice *device, SDL_GPUTexture *texture,
                                  const Uint8 *data, Uint32 width, Uint32 height,
                                  Uint32 mipLevels, bool isCompressed,
                                  size_t blockSize)
    {
        const Uint8 *srcPtr = data;

        for (Uint32 mip = 0; mip < mipLevels; ++mip)
        {
            Uint32 mipWidth = width >> mip;
            Uint32 mipHeight = height >> mip;
            if (mipWidth == 0)
                mipWidth = 1;
            if (mipHeight == 0)
                mipHeight = 1;

            size_t mipSize;
            if (isCompressed)
            {
                Uint32 blocksWide = (mipWidth + 3) / 4;
                Uint32 blocksHigh = (mipHeight + 3) / 4;
                mipSize = blocksWide * blocksHigh * blockSize;
            }
            else
            {
                mipSize = mipWidth * mipHeight * blockSize;
            }

            // Create transfer buffer
            SDL_GPUTransferBufferCreateInfo transferInfo{};
            transferInfo.size = mipSize;
            transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;

            SDL_GPUTransferBuffer *transferBuffer =
                SDL_CreateGPUTransferBuffer(device, &transferInfo);

            if (!transferBuffer)
            {
                SDL_Log("DDS: Failed to create transfer buffer for mip %u", mip);
                return false;
            }

            // Map and copy data
            void *mapped = SDL_MapGPUTransferBuffer(device, transferBuffer, false);
            std::memcpy(mapped, srcPtr, mipSize);
            SDL_UnmapGPUTransferBuffer(device, transferBuffer);

            // Upload to GPU
            SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device);
            SDL_GPUCopyPass *copyPass = SDL_BeginGPUCopyPass(cmd);

            SDL_GPUTextureTransferInfo tti{};
            tti.transfer_buffer = transferBuffer;
            tti.offset = 0;

            SDL_GPUTextureRegion region{};
            region.texture = texture;
            region.mip_level = mip;
            region.x = 0;
            region.y = 0;
            region.z = 0;
            region.w = mipWidth;
            region.h = mipHeight;
            region.d = 1;

            SDL_UploadToGPUTexture(copyPass, &tti, &region, false);

            SDL_EndGPUCopyPass(copyPass);
            SDL_SubmitGPUCommandBuffer(cmd);

            SDL_ReleaseGPUTransferBuffer(device, transferBuffer);

            srcPtr += mipSize;
        }

        return true;
    }
};

// Usage example:
/*
DDSTextureInfo* info = DDSLoader::LoadFromFile(device, "texture.dds");
if (info) {
    // Use info->texture
    SDL_GPUTexture* myTexture = info->texture;

    // When done:
    DDSLoader::Release(device, info);
}
*/