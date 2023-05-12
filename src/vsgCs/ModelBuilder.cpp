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

#include "ModelBuilder.h"

#include "accessor_traits.h"
#include "accessorUtils.h"
#include "DescriptorSetConfigurator.h"
#include "MultisetPipelineConfigurator.h"
#include "LoadGltfResult.h"
#include "pbr.h"
#include "Styling.h"

#include <CesiumGltf/ExtensionKhrTextureBasisu.h>
#include <CesiumGltf/ExtensionTextureWebp.h>

using namespace vsgCs;
using namespace CesiumGltf;

const std::string &Cs3DTilesExtension::getExtensionName() const
{
    static std::string name("CS_3DTiles");
    return name;
};

const std::string &StylingExtension::getExtensionName() const
{
    static std::string name("CS_3DTiles_styling");
    return name;
};


namespace
{
    bool isGltfIdentity(const std::vector<double>& matrix)
    {
        if (matrix.size() != 16)
            return false;
        for (int i = 0; i < 16; ++i)
        {
            if (i % 5 == 0)
            {
                if (matrix[i] != 1.0)
                {
                    return false;
                }
            }
            else
            {
                if (matrix[i] != 0.0)
                {
                    return false;
                }
            }
        }
        return true;
    }

    // From Cesium Unreal
    /**
     * @brief Constrain the length of the given string.
     *
     * If the string is shorter than the maximum length, it is returned.
     * If it is not longer than 3 characters, the first maxLength
     * characters will be returned.
     * Otherwise, the result will be of the form `prefix + "..." + suffix`,
     * with the prefix and suffix chosen so that the length of the result
     * is maxLength
     *
     * @param s The input string
     * @param maxLength The maximum length.
     * @return The constrained string
     */
    std::string constrainLength(const std::string& s, const size_t maxLength)
    {
        if (s.length() <= maxLength)
        {
            return s;
        }
        if (maxLength <= 3)
        {
            return s.substr(0, maxLength);
        }
        const std::string ellipsis("...");
        const size_t prefixLength = ((maxLength - ellipsis.length()) + 1) / 2;
        const size_t suffixLength = (maxLength - ellipsis.length()) / 2;
        const std::string prefix = s.substr(0, prefixLength);
        const std::string suffix = s.substr(s.length() - suffixLength, suffixLength);
        return prefix + ellipsis + suffix;
    }

    bool generateNormals(vsg::ref_ptr<vsg::vec3Array> positions, vsg::ref_ptr<vsg::vec3Array> normals,
                         VkPrimitiveTopology topology)
    {
        auto setNormals =
            [&](uint32_t p0, uint32_t p1, uint32_t p2)
            {
                vsg::vec3 v0 = (*positions)[p1] - (*positions)[p0];
                vsg::vec3 v1 = (*positions)[p2] - (*positions)[p0];
                vsg::vec3 perp = vsg::cross(v0, v1);
                float len = vsg::length(perp);
                if (len > 0.0f)
                {
                    perp = perp / len;
                }
                else
                {
                    // The edges are parallel and the triangle is degenerate. Try to construct
                    // something perpendicular to the edges.
                    perp = vsg::vec3(-v0.y, v0.x, 0.0);
                    len = vsg::length(perp);
                    if (len > 0.0f)
                    {
                        perp = perp / len;
                    }
                    else
                    {
                        perp = vsg::vec3(0.0f, 1.0f, 0.0f);
                    }
                }
                (*normals)[p0] = perp;
                if (topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
                {
                    (*normals)[p1] = perp;
                    (*normals)[p2] = perp;
                }
            };
        switch (topology)
        {
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
            mapTriangleList(static_cast<uint32_t>(positions->size()), setNormals);
            return true;
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY:
            mapTriangleStrip(static_cast<uint32_t>(positions->size()), setNormals);
            return true;
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
            mapTriangleFan(static_cast<uint32_t>(positions->size()), setNormals);
            return true;
        default:
            return false;
        }
    }
}

inline VkDescriptorSetLayoutBinding getVk(const vsg::UniformBinding& binding)
{
    return VkDescriptorSetLayoutBinding{binding.binding, binding.descriptorType, binding.descriptorCount,
                                        binding.stageFlags, nullptr};
}

CreateModelOptions::CreateModelOptions(bool in_renderOverlays, vsg::ref_ptr<Styling> in_styling)
    : renderOverlays(in_renderOverlays), styling(in_styling)
{
}

CreateModelOptions::~CreateModelOptions()
{
}

ModelBuilder::ModelBuilder(const vsg::ref_ptr<GraphicsEnvironment>& genv, CesiumGltf::Model* model,
                           const CreateModelOptions& options,
                           const ExtensionList& enabledExtensions
    )
    : _genv(genv), _model(model), _options(options),
      _csMaterials(model->materials.size()),
      _loadedImages(model->images.size()),
      _activeExtensions(enabledExtensions)
{
    _name = "glTF";
    auto urlIt = _model->extras.find("Cesium3DTiles_TileUrl");
    if (urlIt != _model->extras.end())
    {
        _name = urlIt->second.getStringOrDefault("glTF");
        _name = constrainLength(_name, 256); // NOLINT
    }
    if (isEnabled<StylingExtension>() && options.styling.valid())
    {
        _stylist = options.styling->getStylist(this);
    }
}

ModelBuilder::~ModelBuilder()
{
}

vsg::ref_ptr<vsg::Group>
ModelBuilder::loadNode(const CesiumGltf::Node* node)
{
    const std::vector<double>& matrix = node->matrix;
    vsg::ref_ptr<vsg::Group> result;
    if (isGltfIdentity(matrix))
    {
        result = vsg::Group::create();
    }
    else
    {
        vsg::dmat4 transformMatrix;
        if (matrix.size() == 16)
        {
            std::copy(matrix.begin(), matrix.end(), transformMatrix.data());
        }
        else
        {
            vsg::dmat4 translation;
            if (node->translation.size() == 3)
            {
                translation = vsg::translate(node->translation[0], node->translation[1], node->translation[2]);
            }
            vsg::dquat rotation(0.0, 0.0, 0.0, 1.0);
            if (node->rotation.size() == 4)
            {
                rotation.x = node->rotation[1];
                rotation.y = node->rotation[2];
                rotation.z = node->rotation[3];
                rotation.w = node->rotation[0];
            }
            vsg::dmat4 scale;
            if (node->scale.size() == 3)
            {
                scale = vsg::scale(node->scale[0], node->scale[1], node->scale[2]);
            }
            transformMatrix = translation * rotate(rotation) * scale;
        }
        result = vsg::MatrixTransform::create(transformMatrix);
    }
    int meshId = node->mesh;
    if (meshId >= 0 && static_cast<unsigned>(meshId) < _model->meshes.size())
    {
        result->addChild(loadMesh(&_model->meshes[meshId]));
    }
    for (int childNodeId : node->children)
    {
        if (childNodeId >= 0 && static_cast<unsigned>(childNodeId) < _model->nodes.size())
        {
            result->addChild(loadNode(&_model->nodes[childNodeId]));
        }
    }
    return result;
}

vsg::ref_ptr<vsg::Group>
ModelBuilder::loadMesh(const CesiumGltf::Mesh* mesh)
{
    auto result = vsg::Group::create();
    int primNum = 0;
    try
    {
        for (const CesiumGltf::MeshPrimitive& primitive : mesh->primitives)
        {
            result->addChild(loadPrimitive(&primitive, mesh));
            ++primNum;
        }
    }
    catch (std::runtime_error& err)
    {
        vsg::warn("Error loading mesh, prim ", primNum, err.what());
    }
    return result;
}

namespace
{
    bool gltfToVk(int32_t mode, VkPrimitiveTopology& topology)
    {
        bool valid = true;
        switch (mode)
        {
        case MeshPrimitive::Mode::POINTS:
            topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
            break;
        case MeshPrimitive::Mode::LINES:
            topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
            break;
        case MeshPrimitive::Mode::LINE_STRIP:
            topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
            break;
        case MeshPrimitive::Mode::TRIANGLES:
            topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            break;
        case MeshPrimitive::Mode::TRIANGLE_STRIP:
            topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            break;
        case MeshPrimitive::Mode::TRIANGLE_FAN:
            topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
            break;
        case MeshPrimitive::Mode::LINE_LOOP:
        default:
            valid = false;
        }
        return valid;
    }

    bool isTriangleTopology(VkPrimitiveTopology& topology)
    {
        return topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
            || topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
            || topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    }

    std::map<int32_t, int32_t>
    findTextureCoordAccessors(const std::string& prefix,
                              const std::unordered_map<std::string, int32_t>& attributes)
    {
        std::map<int32_t, int32_t> result;
        for (auto itr = attributes.begin(); itr != attributes.end(); ++itr)
        {
            const auto& name = itr->first;
            auto texCoords = getUintSuffix(prefix, name);
            if (texCoords)
            {
                result[texCoords.value()] = itr->second;
            }
        }
        return result;
    }

// Copying vertex attributes
// The shader set specifies the attribute format as a VkFormat. Either we supply the data in that
// format, or we have to set the format property of the vsg::Array. The default pbr shader requires
// all its attributes in float format. For now we will convert the data to float, but eventually we
// will want to supply the data in the more compact formats if they are supported by the physical device.
//
// The PBR shader expects color data as RGBA, but that is just too nasty! Set the correct format
// for RGB if that is provided by the glTF asset.


    template<typename T, typename TI>
    vsg::ref_ptr<vsg::Data> colorProcessor(const AccessorView<AccessorTypes::VEC3<T>>& accessorView,
                                           const AccessorView<TI>& indexView)
    {
        vsg::ref_ptr<vsg::Data> result;
        if constexpr (std::is_same<T, float>::value)
        {
            if (indexView.status() == AccessorViewStatus::Valid)
            {
                result = createArray(accessorView, indexView);
            }
            else
            {
                result = createArray(accessorView);
            }
        }
        else
        {
            if (indexView.status() == AccessorViewStatus::Valid)
            {
                result = createNormalized<float>(accessorView, indexView);
            }
            else
            {
                result = createNormalized<float>(accessorView);
            }
        }

        result->properties.format = VK_FORMAT_R32G32B32_SFLOAT;
        return result;
    }

    template<typename T, typename TI>
    vsg::ref_ptr<vsg::Data> colorProcessor(const AccessorView<AccessorTypes::VEC4<T>>& accessorView,
                                           const AccessorView<TI>& indexView)
    {
        vsg::ref_ptr<vsg::Data> result;
        if constexpr (std::is_same<T, float>::value)
        {
            if (indexView.status() == AccessorViewStatus::Valid)
            {
                result = createArray(accessorView, indexView);
            }
            else
            {
                result = createArray(accessorView);
            }
        }
        else
        {
            if (indexView.status() == AccessorViewStatus::Valid)
            {
                result = createNormalized<float>(accessorView, indexView);
            }
            else
            {
                result = createNormalized<float>(accessorView);
            }
        }

        result->properties.format = VK_FORMAT_R32G32B32A32_SFLOAT;
        return result;
    }

    template<typename T, typename TI>
    vsg::ref_ptr<vsg::Data> colorProcessor(const AccessorView<T>&, const AccessorView<TI>&) { return {}; } // invalidView


    vsg::ref_ptr<vsg::Data> doColors(const Model* model,
                                     const Accessor* dataAccessor, const Accessor* indexAccessor)
    {
        return invokeWithAccessorViews<vsg::ref_ptr<vsg::Data>>(model,
                                                                [](auto&& accessorView, auto&&indicesview)
                                                                {
                                                                    return colorProcessor(accessorView, indicesview);
                                                                },
                                                                dataAccessor, indexAccessor);
    }

    template<typename T, typename TI>
    vsg::ref_ptr<vsg::Data> texProcessor(const AccessorView<AccessorTypes::VEC2<T>>& accessorView,
                                         const AccessorView<TI>& indexView)
    {
        vsg::ref_ptr<vsg::Data> result;
        if constexpr (std::is_same<T, float>::value)
        {
            if (indexView.status() == AccessorViewStatus::Valid)
            {
                result = createArray(accessorView, indexView);
            }
            else
            {
                result = createArray(accessorView);
            }
        }
        else
        {
            if (indexView.status() == AccessorViewStatus::Valid)
            {
                result = createNormalized<float>(accessorView, indexView);
            }
            else
            {
                result = createNormalized<float>(accessorView);
            }
        }

        result->properties.format = VK_FORMAT_R32G32_SFLOAT;
        return result;
    }

    template<typename T, typename TI>
    vsg::ref_ptr<vsg::Data> texProcessor(const AccessorView<T>&, const AccessorView<TI>&) { return {}; } // invalidView

    vsg::ref_ptr<vsg::Data> doTextures(const Model* model,
                                       const Accessor* dataAccessor, const Accessor* indexAccessor)
    {
        return invokeWithAccessorViews<vsg::ref_ptr<vsg::Data>>(model,
                                                                [](const auto& accessorView, const auto& indicesview)
                                                                {
                                                                    return texProcessor(accessorView, indicesview);
                                                                },
                                                                dataAccessor, indexAccessor);
    }
}

template <typename A, typename I>
vsg::ref_ptr<vsg::Data> createData(Model* model, const A* dataAccessor, const I* indicesAccessor = nullptr)
{
    if (indicesAccessor)
    {
        return invokeWithAccessorViews<vsg::ref_ptr<vsg::Data>>(
            model,
            [](auto&& dataView, auto&&indicesView)
            {
                return vsg::ref_ptr<vsg::Data>(createArray(dataView, indicesView));
            },
            dataAccessor, indicesAccessor);
    }
    return  createAccessorView(*model, *dataAccessor,
                               [](auto&& accessorView)
                               {
                                   return vsg::ref_ptr<vsg::Data>(createArray(accessorView));
                               });
}

// I naively wrote the below comment:
//
// Lots of hair for this in cesium-unreal. The main issues there seem to be 1) the need to recopy
// indices into a single format in Unreal and 2) support for duplicating vertices for generating
// normals and tangents. 1) isn't much of a problem (though uint8 is only supported with an
// extension), and 2) will be dealt with later. If we need to duplicate vertices, we will do that on
// the VSG data structures.
//
// 500 lines of code later, we have plenty of hair for duplicating vertex data,
// generating normals, recopying into the right data format... Most of our hair
// is in template functions, but there is a kind of conservation of hair going
// on here.

namespace vsgCs
{
    // Helper function for getting an attribute accessor by name.
    const Accessor* getAccessor(const Model* model, const MeshPrimitive* primitive, const std::string& name)
    {
        auto itr = primitive->attributes.find(name);
        if (itr != primitive->attributes.end())
        {
            int accessorID = itr->second;
            return  Model::getSafe(&model->accessors, accessorID);
        }
        return nullptr;
    }
}

// Hack to construct a name for error message purposes, etc.

std::string ModelBuilder::makeName(const CesiumGltf::Mesh *mesh,
                                   const CesiumGltf::MeshPrimitive *primitive) const
{
    std::string name = _name;
    std::ptrdiff_t meshNum = mesh - _model->meshes.data();
    std::ptrdiff_t primNum = primitive - mesh->primitives.data();
    if (meshNum >= 0 && static_cast<size_t>(meshNum) < _model->meshes.size())
    {
        name += " mesh " + std::to_string(meshNum);
    }
    if (primNum >= 0 && static_cast<size_t>(primNum) < mesh->primitives.size())
    {
        name += " primitive " + std::to_string(primNum);
    }
    return name;
}

namespace
{
    vsg::ref_ptr<vsg::vec4Array> expandArray(const Model *model,
                                             const vsg::ref_ptr<vsg::Data>& srcData,
                                             const Accessor* indexAccessor)
    {
        auto src = ref_ptr_cast<vsg::vec4Array>(srcData);
        if (!indexAccessor || !src)
        {
            return src;
        }
        return createAccessorView(
            *model,
            *indexAccessor,
            [src](auto&& indexView)
             {
                 if constexpr(is_index_view<decltype(indexView)>::value)
                 {
                     auto result = vsg::vec4Array::create(indexView.size());
                     for (int i = 0; i < indexView.size(); ++i)
                     {
                         (*result)[i] = (*src)[indexView[i].value[0]];
                     }
                     return result;
                 }
                 return vsg::vec4Array::create();
             });
    }
}

vsg::ref_ptr<vsg::Node>
ModelBuilder::loadPrimitive(const CesiumGltf::MeshPrimitive* primitive,
                            const CesiumGltf::Mesh* mesh)
{
    const Accessor* pPositionAccessor = getAccessor(_model, primitive, "POSITION");
    if (!pPositionAccessor)
    {
        // Position accessor does not exist, so ignore this primitive.
        return {};
    }

    std::string name = makeName(mesh, primitive);
    VkPrimitiveTopology topology;
    if (!gltfToVk(primitive->mode, topology))
    {
        vsg::warn(name, ": Can't map glTF mode ", primitive->mode, " to Vulkan topology");
        return {};
    }
    auto csMaterial = loadMaterial(primitive->material, topology);
    auto descConf = csMaterial->descriptorConfig;
    auto pipelineConf = MultisetPipelineConfigurator::create(descConf->shaderSet);
    pipelineConf->defines() = descConf->defines;
    pipelineConf->inputAssemblyState->topology = topology;
    if (topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST)
    {
        pipelineConf->defines().insert("VSGCS_SIZE_TO_ERROR");
    }
    bool generateTangents = csMaterial->texInfo.count("normalMap") != 0
        && primitive->attributes.count("TANGENT") == 0;
    const Accessor* indicesAccessor = Model::getSafe(&_model->accessors, primitive->indices);
    const Accessor* normalAccessor = getAccessor(_model, primitive, "NORMAL");
    bool hasNormals = normalAccessor != nullptr;
    // The indices will be used to expand the attribute arrays.
    const Accessor* expansionIndices = ((!hasNormals || generateTangents) && indicesAccessor
                                        ? &_model->accessors[primitive->indices] : nullptr);
    Stylist::PrimitiveStyling primStyling;
    if (_stylist)
    {
        primStyling = _stylist->getStyling(primitive);
    }
    if (!primStyling.show)
    {
        return {};
    }
    vsg::DataList vertexArrays;
    auto positions = createData(_model, pPositionAccessor, expansionIndices);
    pipelineConf->assignArray(vertexArrays, "vsg_Vertex", VK_VERTEX_INPUT_RATE_VERTEX, positions);
    if (normalAccessor)
    {
        pipelineConf->assignArray(vertexArrays, "vsg_Normal", VK_VERTEX_INPUT_RATE_VERTEX,
                            ref_ptr_cast<vsg::vec3Array>(createData(_model, normalAccessor, expansionIndices)));
    }
    else if (!isTriangleTopology(pipelineConf->inputAssemblyState->topology)) // Can not make normals
    {
        if (pipelineConf->inputAssemblyState->topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST)
        {
            pipelineConf->defines().insert("VSGCS_BILLBOARD_NORMAL");
        }
        auto normal = vsg::vec3Value::create(vsg::vec3(0.0f, 1.0f, 0.0f));
        pipelineConf->assignArray(vertexArrays, "vsg_Normal", VK_VERTEX_INPUT_RATE_INSTANCE, normal);
    }
    else
    {
        auto posArray = ref_ptr_cast<vsg::vec3Array>(positions);
        auto normals = vsg::vec3Array::create(posArray->size());
        generateNormals(posArray, normals, pipelineConf->inputAssemblyState->topology);
        pipelineConf->defines().insert("VSGCS_FLAT_SHADING");
        pipelineConf->assignArray(vertexArrays, "vsg_Normal", VK_VERTEX_INPUT_RATE_VERTEX, normals);
    }

    // XXX
    // The VSG PBR shader doesn't use vertex tangent data, so we will skip the Cesium Unreal hair
    // for loading tangent data or generating it if necessary.
    //
    // We will probs change the shader to use tangent data.

    // XXX water mask

    // Bounding volumes aren't stored in most nodes in VSG and are computed when needed. Should we
    // store the the position min / max, or just not bother?

    if (primStyling.colors.valid())
    {
        auto styledColors = primStyling.colors;
        if (expansionIndices)
        {
            styledColors = expandArray(_model, primStyling.colors, expansionIndices);
        }
        pipelineConf->assignArray(vertexArrays, "vsg_Color", primStyling.vertexRate, styledColors);
    }
    else
    {
        const Accessor* colorAccessor = getAccessor(_model, primitive, "COLOR_0");
        vsg::ref_ptr<vsg::Data> colorData;
        if (colorAccessor)
        {
            colorData = doColors(_model, colorAccessor, expansionIndices);
        }
        if (!colorData)
        {
            auto color = vsg::vec4Value::create(colorWhite);
            pipelineConf->assignArray(vertexArrays, "vsg_Color", VK_VERTEX_INPUT_RATE_INSTANCE, color);
        }
        else
        {
            pipelineConf->assignArray(vertexArrays, "vsg_Color", VK_VERTEX_INPUT_RATE_VERTEX, colorData);
        }
    }
    // Textures...
    const auto& assignTexCoord = [&](const std::string& texPrefix, int baseLocation)
    {
        std::map<int32_t, int32_t> texAccessors = findTextureCoordAccessors(texPrefix, primitive->attributes);
        for (int i = 0; i < 2; ++i)
        {
            std::string arrayName = "vsg_TexCoord" + std::to_string(i + baseLocation);
            vsg::ref_ptr<vsg::Data> texdata;
            auto texcoordItr = texAccessors.find(i);
            if (texcoordItr != texAccessors.end())
            {
                const Accessor* texAccessor = Model::getSafe(&_model->accessors, texcoordItr->second);
                if (texAccessor)
                {
                    texdata = doTextures(_model, texAccessor, expansionIndices);
                }
            }
            if (texdata.valid())
            {
                pipelineConf->assignArray(vertexArrays, arrayName, VK_VERTEX_INPUT_RATE_VERTEX, texdata);
            }
            else
            {
                auto texcoord = vsg::vec2Value::create(vsg::vec2(0.0f, 0.0f));
                pipelineConf->assignArray(vertexArrays, arrayName, VK_VERTEX_INPUT_RATE_INSTANCE, texcoord);
            }
        }
    };
    assignTexCoord("TEXCOORD_", 0);
    if (isEnabled<Cs3DTilesExtension>())
    {
        assignTexCoord("_CESIUMOVERLAY_", 2);
    }
    vsg::ref_ptr<vsg::Command> drawCommand;
    if (indicesAccessor && !expansionIndices)
    {
        vsg::ref_ptr<vsg::Data> indices = createAccessorView(*_model, *indicesAccessor, IndexVisitor());
        auto vid = vsg::VertexIndexDraw::create();
        vid->assignArrays(vertexArrays);
        vid->assignIndices(indices);
        vid->indexCount = static_cast<uint32_t>(indices->valueCount());
        vid->instanceCount = 1;
        drawCommand = vid;
    }
    else
    {
        auto vd = vsg::VertexDraw::create();
        vd->assignArrays(vertexArrays);
        vd->vertexCount = static_cast<uint32_t>(positions->valueCount());
        vd->instanceCount = 1;
        drawCommand = vd;
    }
    drawCommand->setValue("name", name);
    if (descConf->blending)
    {
        // figure out what this means someday...
        // These are parameters for blending into the first color attachment in the render
        // pass. While this set of parameters implements bog-standard alpha blending, should they be
        // buried at this low level?
        //
        // Note that this will work for any "compatible" render pass too.
        pipelineConf->colorBlendState->attachments = vsg::ColorBlendState::ColorBlendAttachments{
            {true, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD,
             VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_SUBTRACT,
             VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT}};
        _genv->sharedObjects->share(pipelineConf->colorBlendState);
    }
    if (descConf->two_sided)
    {
        pipelineConf->rasterizationState->cullMode = VK_CULL_MODE_NONE;
    }
    if (descConf->descriptorSet)
    {
        pipelineConf->descriptorSetLayout = descConf->descriptorSet->setLayout;
        pipelineConf->descriptorBindings = descConf->descriptorBindings;
    }
    pipelineConf->init();
    _genv->sharedObjects->share(pipelineConf->bindGraphicsPipeline);

    auto stateGroup = vsg::StateGroup::create();
    stateGroup->add(pipelineConf->bindGraphicsPipeline);

    if (descConf->descriptorSet)
    {
        auto bindDescriptorSet
            = vsg::BindDescriptorSet::create(VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineConf->layout, pbr::PRIMITIVE_DESCRIPTOR_SET,
                                             descConf->descriptorSet);
        stateGroup->add(bindDescriptorSet);
    }

    auto bindViewDescriptorSets = vsg::BindViewDescriptorSets::create(VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineConf->layout, pbr::VIEW_DESCRIPTOR_SET);
    stateGroup->add(bindViewDescriptorSets);


    // assign any custom ArrayState that may be required.
    stateGroup->prototypeArrayState = pipelineConf->shaderSet->getSuitableArrayState(pipelineConf->defines());

    stateGroup->addChild(drawCommand);

    vsg::ComputeBounds computeBounds;
    drawCommand->accept(computeBounds);
    vsg::dvec3 center = (computeBounds.bounds.min + computeBounds.bounds.max) * 0.5;
    double radius = vsg::length(computeBounds.bounds.max - computeBounds.bounds.min) * 0.5;

    if (descConf->blending)
    {
        auto depthSorted = vsg::DepthSorted::create();
        depthSorted->binNumber = 10;
        depthSorted->bound.set(center[0], center[1], center[2], radius);
        depthSorted->child = stateGroup;

        return depthSorted;
    }
    auto cullNode = vsg::CullNode::create(vsg::dsphere(center[0], center[1], center[2], radius),
                                          stateGroup);
    return cullNode;
}

vsg::ref_ptr<ModelBuilder::CsMaterial>
ModelBuilder::loadMaterial(const CesiumGltf::Material* material, VkPrimitiveTopology topology)
{
    auto csMat = CsMaterial::create();
    csMat->descriptorConfig = DescriptorSetConfigurator::create();
    // XXX Cesium Unreal always enables two-sided, but it should come from the material...
    csMat->descriptorConfig->two_sided = true;
    csMat->descriptorConfig->defines.insert("VSG_TWO_SIDED_LIGHTING");
    if (_options.renderOverlays)
    {
        csMat->descriptorConfig->defines.insert("VSGCS_OVERLAY_MAPS");
    }
    vsg::PbrMaterial pbr;

    if (material->alphaMode == CesiumGltf::Material::AlphaMode::BLEND)
    {
        csMat->descriptorConfig->blending = true;
        pbr.alphaMaskCutoff = 0.0f;
    }
    csMat->descriptorConfig->shaderSet = _genv->shaderFactory->getShaderSet(topology);
    if (material->pbrMetallicRoughness)
    {
        auto const& cesiumPbr = material->pbrMetallicRoughness.value();
        for (int i = 0; i < 3; ++i)
        {
            pbr.baseColorFactor[i] = static_cast<float>(cesiumPbr.baseColorFactor[i]);
        }
        if (cesiumPbr.baseColorFactor.size() > 3)
        {
            pbr.baseColorFactor[3] = static_cast<float>(cesiumPbr.baseColorFactor[3]);
        }
        if (cesiumPbr.metallicFactor >= 0.0)
        {
            pbr.metallicFactor = static_cast<float>(cesiumPbr.metallicFactor);
        }
        if (cesiumPbr.roughnessFactor >= 0.0)
        {
            pbr.roughnessFactor = static_cast<float>(cesiumPbr.roughnessFactor);
        }
        loadMaterialTexture(csMat, "diffuseMap", cesiumPbr.baseColorTexture, true);
        loadMaterialTexture(csMat, "mrMap", cesiumPbr.metallicRoughnessTexture, false);
    }
    loadMaterialTexture(csMat, "normalMap", material->normalTexture, false);
    loadMaterialTexture(csMat, "aoMap", material->occlusionTexture, false);
    loadMaterialTexture(csMat, "emissiveMap", material->emissiveTexture, true);
    csMat->descriptorConfig->assignUniform("material", vsg::PbrMaterialValue::create(pbr));
    auto descriptorSetLayout
        = vsg::DescriptorSetLayout::create(csMat->descriptorConfig->descriptorBindings);
    csMat->descriptorConfig->descriptorSet
        = vsg::DescriptorSet::create(descriptorSetLayout, csMat->descriptorConfig->descriptors);
    return csMat;
}

vsg::ref_ptr<ModelBuilder::CsMaterial>
ModelBuilder::loadMaterial(int i, VkPrimitiveTopology topology)
{
    int topoIndex = topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST ? 1 : 0;

    if (i < 0 || static_cast<unsigned>(i) >= _model->materials.size())
    {
        if (!_baseMaterial[topoIndex])
        {
            _baseMaterial[topoIndex] = CsMaterial::create();
            _baseMaterial[topoIndex]->descriptorConfig = DescriptorSetConfigurator::create();
            _baseMaterial[topoIndex]->descriptorConfig->shaderSet = _genv->shaderFactory->getShaderSet(topology);
            vsg::PbrMaterial pbr;
            _baseMaterial[topoIndex]->descriptorConfig->assignUniform("material",
                                                                         vsg::PbrMaterialValue::create(pbr));
        }
        return _baseMaterial[topoIndex];
    }
    if (!_csMaterials[i][topoIndex])
    {
        _csMaterials[i][topoIndex] = loadMaterial(&_model->materials[i], topology);
    }
    return _csMaterials[i][topoIndex];
}

vsg::ref_ptr<vsg::Group>
ModelBuilder::operator()()
{
    vsg::ref_ptr<vsg::Group> resultNode = vsg::Group::create();

    if (_model->scene >= 0 && static_cast<unsigned>(_model->scene) < _model->scenes.size())
    {
        // Show the default scene
        const Scene& defaultScene = _model->scenes[_model->scene];
        for (int nodeId : defaultScene.nodes)
        {
            resultNode->addChild(loadNode(&_model->nodes[nodeId]));
        }
    }
    else if (!_model->scenes.empty())
    {
        // There's no default, so show the first scene
        const Scene& defaultScene = _model->scenes[0];
        for (int nodeId : defaultScene.nodes)
        {
            resultNode->addChild(loadNode(&_model->nodes[nodeId]));
        }
    }
    else if (!_model->nodes.empty())
    {
        // No scenes at all, use the first node as the root node.
        resultNode = loadNode(_model->nodes.data());
    }
    else if (!_model->meshes.empty())
    {
        // No nodes either, show all the meshes.
        for (const Mesh& mesh : _model->meshes)
        {
            resultNode->addChild(loadMesh(&mesh));
        }
    }
    return resultNode;
}

vsg::ref_ptr<vsg::Data> ModelBuilder::loadImage(int i, bool useMipMaps, bool sRGB)
{
    CesiumGltf::ImageCesium& image = _model->images[i].cesium;
    ImageData& imageData = _loadedImages[i];
    if ((imageData.image.valid() || imageData.imageWithMipmap.valid())
        && sRGB != imageData.sRGB)
    {
        vsg::warn(_name, ": image ", i, " used as linear and sRGB");
    }
    if (imageData.imageWithMipmap.valid())
    {
        return imageData.imageWithMipmap;
    }
    if (!useMipMaps && imageData.image.valid())
    {
        return imageData.image;
    }
    auto data = vsgCs::loadImage(image, useMipMaps, sRGB);
    imageData.sRGB = sRGB;
    if (useMipMaps)
    {
        return imageData.imageWithMipmap = data;
    }
    else
    {
        return imageData.image = data;
    }
}

// helper to simplify index validation logic
namespace
{
    template<typename T>
    std::optional<int32_t> safeIndex(const std::vector<T>& items, int32_t index)
    {
        if (index >= 0 || static_cast<uint32_t>(index) < items.size())
        {
            return index;
        }
        return {};
    }
}

vsg::ref_ptr<vsg::ImageInfo> ModelBuilder::loadTexture(const CesiumGltf::Texture& texture,
                                      bool sRGB)
{
    const CesiumGltf::ExtensionKhrTextureBasisu* pKtxExtension =
        texture.getExtension<CesiumGltf::ExtensionKhrTextureBasisu>();
    const CesiumGltf::ExtensionTextureWebp* pWebpExtension =
        texture.getExtension<CesiumGltf::ExtensionTextureWebp>();

    std::optional<int32_t> source;
    if (pKtxExtension)
    {
        if (!(source = safeIndex(_model->images, pKtxExtension->source)))
        {
            vsg::warn("KTX texture source index must be non-negative and less than ",
                      _model->images.size(),
                      " but is ",
                      pKtxExtension->source);
            return {};
        }
    }
    else if (pWebpExtension)
    {
        if (!(source = safeIndex(_model->images, pWebpExtension->source)))
        {
            vsg::warn("WebP texture source index must be non-negative and less than ",
                      _model->images.size(),
                      " but is ",
                      pWebpExtension->source);
            return {};
        }
    }
    else
    {
        if (!(source = safeIndex(_model->images, texture.source)))
        {
            vsg::warn("Texture source index must be non-negative and less than ",
                      _model->images.size(),
                      " but is ",
                      texture.source);
            return {};
        }
    }

    const CesiumGltf::Sampler* pSampler =
        CesiumGltf::Model::getSafe(&_model->samplers, texture.sampler);
    // glTF spec: "When undefined, a sampler with repeat wrapping and auto
    // filtering should be used."
    VkSamplerAddressMode addressX = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkSamplerAddressMode addressY = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkFilter minFilter = VK_FILTER_LINEAR;
    VkFilter magFilter = VK_FILTER_LINEAR;
    bool useMipMaps = false;

    if (pSampler)
    {
        switch (pSampler->wrapS)
        {
        case CesiumGltf::Sampler::WrapS::CLAMP_TO_EDGE:
            addressX = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            break;
        case CesiumGltf::Sampler::WrapS::MIRRORED_REPEAT:
            addressX = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            break;
        case CesiumGltf::Sampler::WrapS::REPEAT:
            addressX = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            break;
        }
        switch (pSampler->wrapT)
        {
        case CesiumGltf::Sampler::WrapT::CLAMP_TO_EDGE:
            addressY = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            break;
        case CesiumGltf::Sampler::WrapT::MIRRORED_REPEAT:
            addressY = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            break;
        case CesiumGltf::Sampler::WrapT::REPEAT:
            addressY = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            break;
        }
        if (pSampler->magFilter
            && pSampler->magFilter.value() == CesiumGltf::Sampler::MagFilter::NEAREST)
        {
            magFilter = VK_FILTER_NEAREST;
        }
        if (pSampler->minFilter)
        {
            switch (pSampler->minFilter.value()) {
            case CesiumGltf::Sampler::MinFilter::NEAREST:
            case CesiumGltf::Sampler::MinFilter::NEAREST_MIPMAP_NEAREST:
                minFilter = VK_FILTER_NEAREST;
                break;
            case CesiumGltf::Sampler::MinFilter::LINEAR:
            case CesiumGltf::Sampler::MinFilter::LINEAR_MIPMAP_NEAREST:
            default:
                break;
            }
        }
        switch (pSampler->minFilter.value_or(
                    CesiumGltf::Sampler::MinFilter::LINEAR_MIPMAP_LINEAR))
        {
        case CesiumGltf::Sampler::MinFilter::LINEAR_MIPMAP_LINEAR:
        case CesiumGltf::Sampler::MinFilter::LINEAR_MIPMAP_NEAREST:
        case CesiumGltf::Sampler::MinFilter::NEAREST_MIPMAP_LINEAR:
        case CesiumGltf::Sampler::MinFilter::NEAREST_MIPMAP_NEAREST:
            useMipMaps = true;
            break;
        default: // LINEAR and NEAREST
            useMipMaps = false;
            break;
        }
    }
    auto data = loadImage(*source, useMipMaps, sRGB);
    auto sampler = makeSampler(addressX, addressY, minFilter, magFilter,
                               samplerLOD(data, useMipMaps));
    _genv->sharedObjects->share(sampler);
    return vsg::ImageInfo::create(sampler, data);
}