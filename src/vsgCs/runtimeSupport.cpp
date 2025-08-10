/* <editor-fold desc="MIT License">

Copyright(c) 2023 Timothy Moore

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

</editor-fold> */

#include "CesiumGltfBuilder.h"
#include "CompilableImage.h"
#include "OpThreadTaskProcessor.h"
#include "Tracing.h"
#include "runtimeSupport.h"
#include "RuntimeEnvironment.h"

#include <Cesium3DTilesContent/registerAllTileContentTypes.h>
#include <Cesium3DTilesSelection/BoundingVolume.h>
#include <Cesium3DTilesSelection/Tile.h>
#include <CesiumAsync/IAssetResponse.h>
#include <CesiumGeometry/OrientedBoundingBox.h>
#include <CesiumGltfReader/ImageDecoder.h>

#include <vsg/core/Allocator.h>

#include <cstdint>
#include <cstdlib>
#include <type_traits>
#include <vsg/core/Data.h>
#include <vsg/maths/vec2.h>
#include <vsg/maths/vec4.h>

namespace vsgCs
{
    using namespace Cesium3DTilesSelection;

    OPENSSL_INIT_SETTINGS* opensslSettings = nullptr;

    void startup()
    {
        Cesium3DTilesContent::registerAllTileContentTypes();
    }

    void shutdown()
    {
        getAsyncSystemWrapper().shutdown();
    }

    vsg::ref_ptr<vsg::LookAt> makeLookAtFromTile(const Cesium3DTilesSelection::Tile* tile,
                                                 double distance,
                                                 bool localModel)
    {
        auto boundingVolume = tile->getBoundingVolume();
        if (localModel)
        {
            CesiumGeometry::OrientedBoundingBox csBoundingBox
                = getOrientedBoundingBoxFromBoundingVolume(boundingVolume);
            CesiumGeometry::BoundingSphere csSphere = csBoundingBox.toSphere();
            vsg::dvec3 vPosition = glm2vsg(csSphere.getCenter());
            double radius = csSphere.getRadius();
            vsg::dvec3 eye = vsg::dvec3(0.0, -2.0 * radius, 0.0);
            vsg::dvec3 up(0.0, 0.0, 1.0);
            return vsg::LookAt::create(vPosition + eye, vPosition, up);
        }
        auto globeRectangle = estimateGlobeRectangle(boundingVolume);
        if (globeRectangle)
        {
            auto cartoCenter = globeRectangle->computeCenter();
            // The geographic coordinates specify a normal to the ellipsoid. How convenient!
            auto normal = CesiumGeospatial::Ellipsoid::WGS84.geodeticSurfaceNormal(cartoCenter);
            auto position = CesiumGeospatial::Ellipsoid::WGS84.cartographicToCartesian(cartoCenter);
            auto vNormal = glm2vsg(normal);
            auto vPosition = glm2vsg(position);
            auto geoCenter = glm2vsg(getBoundingVolumeCenter(boundingVolume));
            if (distance == std::numeric_limits<double>::max())
            {
                distance = vsg::length(vPosition - geoCenter) * 3.0;
            }
            vsg::dvec3 eye = vPosition +  vNormal * distance;
            vsg::dvec3 direction = -vNormal;
            // Try to align up with North
            auto side = vsg::cross(vsg::dvec3(0.0, 0.0, 1.0), direction);
            side = vsg::normalize(side);
            vsg::dvec3 up = vsg::cross(direction, side);
            return vsg::LookAt::create(eye, vPosition, up);
        }
        return vsg::LookAt::create();
    }

    std::string readFile(const vsg::Path& filename, vsg::ref_ptr<const vsg::Options> options)
    {
        if (!options)
        {
            options = RuntimeEnvironment::get()->options;
        }
        vsg::Path filePath = vsg::findFile(filename, options);
        if (filePath.empty())
        {
            throw std::runtime_error("Could not find " + filename.string());
        }
        std::fstream f(filePath);
        std::stringstream iss;
        iss << f.rdbuf();
        return iss.str();
    }

    std::vector<std::byte> readBinaryFile(const vsg::Path& filename,
                                          vsg::ref_ptr<const vsg::Options> options)
    {
        if (!options)
        {
            options = RuntimeEnvironment::get()->options;
        }
        vsg::Path filePath = vsg::findFile(filename, options);
        if (filePath.empty())
        {
            throw std::runtime_error("Could not find " + filename.string());
        }
        std::ifstream input( filePath, std::ios::binary);
        std::vector<std::byte> result;

        std::transform(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>( ),
                       std::back_inserter(result),
                       [](char c)
                       {
                           return static_cast<std::byte>(c);
                       });
        return result;
    }

    int samplerLOD(const vsg::ref_ptr<vsg::Data>& data, bool generateMipMaps)
    {
        int dataMipMaps = data->properties.maxNumMipmaps > 1;
        if (dataMipMaps > 1)
        {
            return dataMipMaps;
        }
        uint32_t width = data->width();
        uint32_t height = data->height();
        if (!generateMipMaps || (width <= 1 && height <= 1))
        {
            return 1;
        }
        auto maxDim = std::max(width, height);
        // from assimp loader; is it correct?
        return std::floor(std::log2f(static_cast<float>(maxDim)));
    }

    vsg::ref_ptr<vsg::Sampler> makeSampler(VkSamplerAddressMode addressX,
                                           VkSamplerAddressMode addressY,
                                           VkFilter minFilter,
                                           VkFilter maxFilter,
                                           int maxNumMipMaps)
    {
        auto result = vsg::Sampler::create();
        if (maxNumMipMaps > 1)
        {
            result->mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        }
        else
        {
            result->mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        }
        result->minFilter = minFilter;
        result->magFilter = maxFilter;
        result->addressModeU = addressX;
        result->addressModeV = addressY;
        result->addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        result->anisotropyEnable = VK_TRUE;
        result->maxAnisotropy = 16.0f;
        result->maxLod = static_cast<float>(maxNumMipMaps);
        return result;
    }

    vsg::ref_ptr<vsg::ImageInfo> makeImage(std::span<const std::byte> data,
                                           bool useMipMaps, bool sRGB,
                                           VkSamplerAddressMode addressX,
                                           VkSamplerAddressMode addressY,
                                           VkFilter minFilter,
                                           VkFilter maxFilter)
    {
        auto env = RuntimeEnvironment::get();
        CesiumGltfReader::ImageReaderResult result
            = CesiumGltfReader::ImageDecoder::readImage(data, env->features.ktx2TranscodeTargets);
        if (!result.pImage)
        {
            vsg::warn("Could not read image data :");
            for (auto& msg : result.errors)
            {
                vsg::warn("readImage: ", msg);
            }
            return {};
        }
        auto imageData = loadImage(result.pImage, useMipMaps, sRGB);
        auto sampler = makeSampler(addressX, addressY, minFilter, maxFilter,
                                   samplerLOD(imageData, useMipMaps));
        env->options->sharedObjects->share(sampler);
        return vsg::ImageInfo::create(sampler, imageData);
    }

    std::optional<uint32_t> getUintSuffix(const std::string& prefix, const std::string& data)
    {
        if (prefix.size() >= data.size())
        {
            return {};
        }
        auto match = std::mismatch(prefix.begin(), prefix.end(), data.begin());
        if (match.first == prefix.end())
        {
            long val = std::strtol(&(*match.second), nullptr, 10);
            return static_cast<uint32_t>(val);
        }
        return {};
    }

    CesiumAsync::Future<ReadRemoteImageResult> readRemoteImage(const std::string& url, bool compile)
    {
        const static std::vector<CesiumAsync::IAssetAccessor::THeader> headers;
        auto env = RuntimeEnvironment::get();
        return env->getAssetAccessor()
            ->get(getAsyncSystem(), url, headers)
            .thenInWorkerThread(
                [env, compile](std::shared_ptr<CesiumAsync::IAssetRequest>&& pRequest)
                {
                    const CesiumAsync::IAssetResponse* pResponse = pRequest->response();
                    if (pResponse == nullptr)
                    {
                        return ReadRemoteImageResult{{},
                                                     {"Image request for " + pRequest->url() + " failed."}};
                    }
                    if (pResponse->statusCode() != 0 &&
                        (pResponse->statusCode() < 200 ||
                         pResponse->statusCode() >= 300))
                    {
                        std::string message = "Image response code " +
                            std::to_string(pResponse->statusCode()) +
                            " for " + pRequest->url();
                        return ReadRemoteImageResult{{}, {message}};
                    }
                    if (pResponse->data().empty())
                    {
                        return ReadRemoteImageResult{{},
                                                     {"Image response for " + pRequest->url()
                                                      + " is empty."}};
                    }
                    auto imageInfo = makeImage(pResponse->data(), true, true);
                    if (!imageInfo)
                    {
                        return ReadRemoteImageResult{{}, {"makeImage failed"}};
                    }
                    if (compile)
                    {
                        auto compilable = CompilableImage::create(imageInfo);
                        // The CompileResult details shouldn't be relevant to compiling an image.
                        auto compileResult = env->getViewer()->compileManager->compile(compilable);
                        if (!compileResult)
                        {
                            return ReadRemoteImageResult{{}, {"makeImage failed"}};
                        }
                    }
                    return ReadRemoteImageResult{imageInfo, {}};
                });
    }

    CesiumAsync::Future<ReadImGuiTextureResult> readRemoteTexture(const std::string &url, bool compile)
    {
        return readRemoteImage(url, false)
            .thenImmediately([compile](ReadRemoteImageResult&& imageResult)
            {
                if (!imageResult.info)
                {
                    return ReadImGuiTextureResult{{}, imageResult.errors};
                }
                static const vsg::DescriptorSetLayoutBindings descriptorBindings{
                    // { binding, descriptorTpe, descriptorCount, stageFlags, pImmutableSamplers}
                    {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
                static auto descriptorSetLayout = vsg::DescriptorSetLayout::create(descriptorBindings);
                auto retval = vsgImGui::Texture::create();
                retval->height = imageResult.info->imageView->image->data->height();
                retval->width = imageResult.info->imageView->image->data->width();
                auto di = vsg::DescriptorImage::create(imageResult.info, 0, 0);
                retval->descriptorSet = vsg::DescriptorSet::create(descriptorSetLayout, vsg::Descriptors{di});
                if (compile)
                {
                    auto env = RuntimeEnvironment::get();
                    env->getViewer()->compileManager->compile(retval);
                }
                return ReadImGuiTextureResult{retval, {}};
            });
    }

} // namespace vsgCs

namespace
{
    VkFormat cesiumToVk(const CesiumGltf::ImageAsset& image, bool sRGB)
    {
        using namespace CesiumGltf;
        auto chooseSRGB = [sRGB](VkFormat sRGBFormat, VkFormat normFormat)
        {
            return sRGB ? sRGBFormat : normFormat;
        };

        if (image.compressedPixelFormat == GpuCompressedPixelFormat::NONE)
        {
            switch (image.channels) {
            case 1:
                return chooseSRGB(VK_FORMAT_R8_SRGB, VK_FORMAT_R8_UNORM);
            case 2:
                return chooseSRGB(VK_FORMAT_R8G8_SRGB, VK_FORMAT_R8G8_UNORM);
            case 3:
                return chooseSRGB(VK_FORMAT_R8G8B8_SRGB, VK_FORMAT_R8G8B8_UNORM);
            case 4:
            default:
                return chooseSRGB(VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM);
            }
        }
        switch (image.compressedPixelFormat)
        {
        case GpuCompressedPixelFormat::ETC1_RGB:
            // ETC1 is a subset of ETC2
            return chooseSRGB(VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK);
        case GpuCompressedPixelFormat::ETC2_RGBA:
            return chooseSRGB(VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK, VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK);
        case GpuCompressedPixelFormat::BC1_RGB:
            return chooseSRGB(VK_FORMAT_BC1_RGB_SRGB_BLOCK, VK_FORMAT_BC1_RGB_UNORM_BLOCK);
        case GpuCompressedPixelFormat::BC3_RGBA:
            return chooseSRGB(VK_FORMAT_BC3_SRGB_BLOCK, VK_FORMAT_BC3_UNORM_BLOCK);
        case GpuCompressedPixelFormat::BC4_R:
            return chooseSRGB(VK_FORMAT_UNDEFINED, VK_FORMAT_BC4_UNORM_BLOCK);
        case GpuCompressedPixelFormat::BC5_RG:
            return chooseSRGB(VK_FORMAT_UNDEFINED, VK_FORMAT_BC5_UNORM_BLOCK);
        case GpuCompressedPixelFormat::BC7_RGBA:
            return chooseSRGB(VK_FORMAT_BC7_SRGB_BLOCK, VK_FORMAT_BC7_UNORM_BLOCK);
        case GpuCompressedPixelFormat::ASTC_4x4_RGBA:
            return chooseSRGB(VK_FORMAT_ASTC_4x4_SRGB_BLOCK, VK_FORMAT_ASTC_4x4_UNORM_BLOCK);
        case GpuCompressedPixelFormat::PVRTC2_4_RGBA:
            return chooseSRGB(VK_FORMAT_PVRTC2_4BPP_SRGB_BLOCK_IMG, VK_FORMAT_PVRTC2_4BPP_UNORM_BLOCK_IMG);
        case GpuCompressedPixelFormat::ETC2_EAC_R11:
            return chooseSRGB(VK_FORMAT_UNDEFINED, VK_FORMAT_EAC_R11_UNORM_BLOCK);
        case GpuCompressedPixelFormat::ETC2_EAC_RG11:
            return chooseSRGB(VK_FORMAT_UNDEFINED, VK_FORMAT_EAC_R11G11_UNORM_BLOCK);
        default:
            // Unsupported compressed texture format.
            return VK_FORMAT_UNDEFINED;
        }
    }

    using BlockSize = std::tuple<uint8_t, uint8_t>;
    BlockSize
    getBlockSize(VkFormat format)
    {
        switch (format)
        {
        case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:

        case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK:

        case VK_FORMAT_BC3_SRGB_BLOCK:
        case VK_FORMAT_BC3_UNORM_BLOCK:

        case VK_FORMAT_BC4_UNORM_BLOCK:

        case VK_FORMAT_BC5_UNORM_BLOCK:

        case VK_FORMAT_BC7_SRGB_BLOCK:
        case VK_FORMAT_BC7_UNORM_BLOCK:

        case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:
        case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:

        case VK_FORMAT_PVRTC2_4BPP_SRGB_BLOCK_IMG:
        case VK_FORMAT_PVRTC2_4BPP_UNORM_BLOCK_IMG:

        case VK_FORMAT_EAC_R11_UNORM_BLOCK:

        case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
            return {4, 4};
        default:
            return {1, 1};
        }
    }

    vsg::ref_ptr<vsg::Data>
    makeArray(uint32_t width, uint32_t height, vsg::Data::Properties in_properties, void* data)
    {
        auto [block_width, block_height] = getBlockSize(in_properties.format);
        auto mem_width = width / block_width;
        auto mem_height = height / block_height;
        auto maker = [&](auto typedData) -> vsg::ref_ptr<vsg::Data>
        {
            using elementType = std::remove_pointer_t<decltype(typedData)>;
            if (typedData)
            {
                return vsg::Array2D<elementType>::create(mem_width, mem_height, typedData, in_properties);
            }
            return vsg::Array2D<elementType>::create(mem_width, mem_height, in_properties);
        };
        switch (in_properties.format)
        {
        case VK_FORMAT_R8_SRGB:
        case VK_FORMAT_R8_UNORM:
            return maker(reinterpret_cast<uint8_t*>(data));
        case VK_FORMAT_R8G8_SRGB:
        case VK_FORMAT_R8G8_UNORM:
            return maker(reinterpret_cast<vsg::ubvec2*>(data));
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_R8G8B8A8_UNORM:
            return maker(reinterpret_cast<vsg::ubvec4*>(data));
        case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
        case VK_FORMAT_BC4_UNORM_BLOCK:
        case VK_FORMAT_PVRTC2_4BPP_SRGB_BLOCK_IMG:
        case VK_FORMAT_PVRTC2_4BPP_UNORM_BLOCK_IMG:
        case VK_FORMAT_EAC_R11_UNORM_BLOCK:
            return maker(reinterpret_cast<vsg::block64*>(data));
        case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
        case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
        case VK_FORMAT_BC3_SRGB_BLOCK:
        case VK_FORMAT_BC3_UNORM_BLOCK:
        case VK_FORMAT_BC5_UNORM_BLOCK:
        case VK_FORMAT_BC7_SRGB_BLOCK:
        case VK_FORMAT_BC7_UNORM_BLOCK:
        case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:
        case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:
        case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
            return maker(reinterpret_cast<vsg::block128*>(data));
        default:
            return {};
        }
    }

    void rgbExpand(CesiumGltf::ImageAsset& image, void* destData)
    {
        VSGCS_ZONESCOPED;
        auto* pDest = static_cast<uint8_t*>(destData);
        auto srcItr = image.pixelData.begin();
        while (srcItr != image.pixelData.end())
        {
            for (int i = 0; i < 3; ++i)
            {
                *pDest++ = std::to_integer<uint8_t>(*srcItr++);
            }
            *pDest++ = 1;
        }
    }
}

namespace vsgCs
{

vsg::ref_ptr<vsg::Data> loadImage(CesiumUtility::IntrusivePointer<CesiumGltf::ImageAsset> image, bool useMipMaps, bool sRGB)
{
    VSGCS_ZONESCOPED;
    if (image->pixelData.empty() || image->width == 0 || image->height == 0)
    {
        return {};
    }
    VkFormat pixelFormat = cesiumToVk(*image, sRGB);
    if (pixelFormat == VK_FORMAT_UNDEFINED)
    {
        return {};
    }
    vsg::Data::Properties props;
    // Assume that the ImageCesium raw format will be fine to upload into Vulkan, except for
    // R8G8B8 uncompressed textures, which are rarely supported.
    bool expand = false;
    if (pixelFormat == VK_FORMAT_R8G8B8_UNORM)
    {
        pixelFormat = VK_FORMAT_R8G8B8A8_UNORM;
        expand = true;
    }
    else if (pixelFormat == VK_FORMAT_R8G8B8_SRGB)
    {
        pixelFormat = VK_FORMAT_R8G8B8A8_SRGB;
        expand = true;
    }
    props.format = pixelFormat;
    if (useMipMaps)
    {
        props.maxNumMipmaps = static_cast<uint8_t>(std::max(image->mipPositions.size(), static_cast<size_t>(1)));
    }
    else
    {
        props.maxNumMipmaps = 1;
    }
    std::tie(props.blockWidth, props.blockHeight) = getBlockSize(pixelFormat);
    props.origin = vsg::BOTTOM_LEFT;
    vsg::ref_ptr<vsg::Data> result;
    if (expand)
    {
        result = makeArray(image->width, image->height, props, nullptr);
        rgbExpand(*image, result->dataPointer());
    }
    else
    {
        props.allocatorType = vsg::ALLOCATOR_TYPE_NO_DELETE;
        result = makeArray(image->width, image->height, props, image->pixelData.data());
        result->setObject("cesiumObject", IntrusivePointerContainer<CesiumGltf::ImageAsset>::create(image));;
    }
    return result;
}
    std::string getTileUrl(const vsg::Object* obj)
    {
        std::string result;
        obj->getValue("tileUrl", result);
        return result;
    }

    std::string toLower(const std::string& input)
    {
        std::string output = input;
        std::transform(output.begin(), output.end(), output.begin(), ::tolower);
        return output;
    }

    std::string& replace_in_place(std::string& s,
                                  const std::string& sub,
                                  const std::string& other)
    {
        if (sub.empty()) return s;
        size_t b = 0;
        for (; ; )
        {
            b = s.find(sub, b);
            if (b == std::string::npos) break;
            s.replace(b, sub.size(), other);
            b += other.size();
        }
        return s;
    }

    static thread_local bool _isMainThread = false;

    bool isMainThread()
    {
        return _isMainThread;
    }

    void setMainThread()
    {
        _isMainThread = true;
    }

    bool isRemoteUrl(const std::string &str)
    {
        return str.starts_with("https:") || str.starts_with("http:");
    }

    bool isUrl(const std::string &str)
    {
        return isRemoteUrl(str) || str.starts_with("file:");
    }
}
