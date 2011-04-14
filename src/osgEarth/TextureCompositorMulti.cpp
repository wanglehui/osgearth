/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
 * Copyright 2008-2010 Pelican Mapping
 * http://osgearth.org
 *
 * osgEarth is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */
#include <osgEarth/TextureCompositorMulti>
#include <osgEarth/ImageUtils>
#include <osgEarth/Registry>
#include <osgEarth/ShaderComposition>
#include <osgEarth/ShaderUtils>
#include <osg/Texture2D>
#include <osg/TexEnv>
#include <osg/TexEnvCombine>
#include <vector>

using namespace osgEarth;

#define LC "[TextureCompositorMultiTexture] "

//------------------------------------------------------------------------

namespace
{
    static osg::Shader*
    s_createTextureVertexShader( const TextureLayout& layout, bool blending )
    {
        std::stringstream buf;
       
        const TextureLayout::TextureSlotVector& slots = layout.getTextureSlots();

        if ( blending )
            buf << "uniform vec4 osgearth_texCoordFactors;\n\n";

        buf << "void osgearth_vert_setupTexturing() \n"
            << "{ \n";

        // to support LOD blending:
        if ( blending )
        {
            buf << "    mat4 texMat = mat4(1.0);\n"
                << "    texMat[0][0] = osgearth_texCoordFactors[0];\n"
                << "    texMat[1][1] = osgearth_texCoordFactors[1];\n"
                << "    texMat[3][0] = osgearth_texCoordFactors[2];\n"
                << "    texMat[3][1] = osgearth_texCoordFactors[3];\n";
        }

        // Set up the texture coordinates for each active slot (primary and secondary).
        // Primary slots are the actual image layer's texture image unit. A Secondary
        // slot is what an image layer uses for LOD blending, and is set on a per-layer basis.
        for( int slot = 0; slot < (int)slots.size(); ++slot )
        {
            if ( slots[slot] >= 0 )
            {
                UID uid = slots[slot];
                int primarySlot = layout.getSlot(uid, 0);

                if ( slot == primarySlot )
                {
                    // normal unit:
                    buf << "    gl_TexCoord["<< slot <<"] = gl_MultiTexCoord" << slot << ";\n";
                }
                else
                {
                    // secondary (blending) unit:
                    buf << "    gl_TexCoord["<< slot <<"] = texMat * gl_MultiTexCoord" << primarySlot << ";\n";
                }
            }
        }

#if 0
        if (blending)
        {
            buf << "    mat4 texMat = mat4(1.0);\n"
                << "    texMat[0][0] = osgearth_texCoordFactors[0];\n"
                << "    texMat[1][1] = osgearth_texCoordFactors[1];\n"
                << "    texMat[3][0] = osgearth_texCoordFactors[2];\n"
                << "    texMat[3][1] = osgearth_texCoordFactors[3];\n";
            for (int i = 0, j = numSlots; i < numSlots; ++i, ++j)
                buf << "    gl_TexCoord[" << i << "] = gl_MultiTexCoord" << i << "; \n"
                    << "    gl_TexCoord[" << j << "] = texMat * gl_MultiTexCoord" << i << ";\n";
        }
        else
        {
            for(int i = 0; i< numSlots; ++i )
                buf << "    gl_TexCoord["<< i <<"] = gl_MultiTexCoord"<< i << "; \n";
        }
#endif
            
        buf << "} \n";

        std::string str = buf.str();
        return new osg::Shader( osg::Shader::VERTEX, str );
    }

    static osg::Shader*
    s_createTextureFragShaderFunction( const TextureLayout& layout, int maxSlots, bool blending, float blendTime )
    {
        const TextureLayout::RenderOrderVector& order = layout.getRenderOrder();

        std::stringstream buf;

        buf << "#version 120 \n";

        if ( blending )
        {
            buf << "#extension GL_ARB_shader_texture_lod : enable \n"
                << "uniform float osgearth_SlotStamp[" << maxSlots << "]; \n"
                << "uniform float osg_FrameTime; \n";
        }

        buf << "uniform float osgearth_ImageLayerOpacity[" << maxSlots << "]; \n"
            << "uniform bool  osgearth_ImageLayerEnabled[" << maxSlots << "]; \n"
            << "uniform float osgearth_ImageLayerRange[" << 2 * maxSlots << "]; \n"
            << "uniform float osgearth_ImageLayerAttenuation; \n"
            << "uniform float osgearth_LODRangeFactor;\n"
            << "uniform float osgearth_CameraElevation; \n"
            << "varying float osgearth_CameraRange; \n";

        const TextureLayout::TextureSlotVector& slots = layout.getTextureSlots();

        for( int i = 0; i < maxSlots && i < (int)slots.size(); ++i )
        {
            if ( slots[i] >= 0 )
            {
                buf << "uniform sampler2D tex" << i << ";\n";
            }
        }

        buf << "void osgearth_frag_applyTexturing( inout vec4 color ) \n"
            << "{ \n"
            << "    vec3 color3 = color.rgb; \n"
            << "    vec4 texel; \n"
            << "    float dmin, dmax, atten_min, atten_max, age; \n";

        for( unsigned int i=0; i < order.size(); ++i )
        {
            int slot = order[i];
            int q = 2 * i;

            // if this UID has a secondyar slot, LOD blending ON.
            int secondarySlot = layout.getSlot( slots[slot], 1, maxSlots );

            buf << "    if (osgearth_ImageLayerEnabled["<< i << "]) { \n"
                << "        dmin = osgearth_CameraElevation - osgearth_ImageLayerRange["<< q << "]; \n"
                << "        dmax = osgearth_CameraElevation - osgearth_ImageLayerRange["<< q+1 <<"]; \n"

                << "        if (dmin >= 0 && dmax <= 0.0) { \n"
                << "            atten_max = -clamp( dmax, -osgearth_ImageLayerAttenuation, 0 ) / osgearth_ImageLayerAttenuation; \n"
                << "            atten_min =  clamp( dmin, 0, osgearth_ImageLayerAttenuation ) / osgearth_ImageLayerAttenuation; \n";

            if ( secondarySlot >= 0 )
            {
                float invBlendTime = 1.0f/blendTime;

                buf << "            age = "<< invBlendTime << " * min( "<< blendTime << ", osg_FrameTime - osgearth_SlotStamp[" << slot << "] ); \n"
                    << "            age = clamp(age, 0.0, 1.0); \n"
                    << "            vec4 texel0 = texture2D(tex" << slot << ", gl_TexCoord["<< slot << "].st);\n"
                    << "            vec4 texel1 = texture2D(tex" << secondarySlot << ", gl_TexCoord["<< secondarySlot << "].st);\n"
                    << "            float mixval = age * osgearth_LODRangeFactor;\n"

                    // pre-multiply alpha before mixing:
                    << "            texel0.rgb *= texel0.a; \n"
                    << "            texel1.rgb *= texel1.a; \n"
                    << "            texel = mix(texel1, texel0, mixval); \n"

                    // revert to non-pre-multiplies alpha (assumes openGL state uses non-pre-mult alpha)
                    << "            if (texel.a > 0.0) { \n"
                    << "                texel.rgb /= texel.a; \n"
                    << "            } \n";
            }
            else
            {
                buf << "            texel = texture2D(tex" << slot << ", gl_TexCoord["<< slot <<"].st); \n";
            }
                    
            buf << "            color3 = mix(color3, texel.rgb, texel.a * osgearth_ImageLayerOpacity[" << i << "] * atten_max * atten_min); \n"
                << "        } \n"
                << "    } \n";
        }

        buf << "    color = vec4(color3,color.a); \n"
            << "} \n";

        std::string str = buf.str();
        //OE_INFO << std::endl << str;
        return new osg::Shader( osg::Shader::FRAGMENT, str );
    }
}

//------------------------------------------------------------------------

namespace
{
    static std::string makeSamplerName(int slot)
    {
        std::stringstream buf;
        buf << "tex" << slot;
        return buf.str();
    }
    
    static osg::Texture2D*
    s_getTexture( osg::StateSet* stateSet, UID layerUID, const TextureLayout& layout, osg::StateSet* parentStateSet)
    {
        int slot = layout.getSlot( layerUID, 0 );
        if ( slot < 0 )
            return 0L;

        osg::Texture2D* tex = static_cast<osg::Texture2D*>(
            stateSet->getTextureAttribute( slot, osg::StateAttribute::TEXTURE ) );

        if ( !tex )
        {
            tex = new osg::Texture2D();

            // configure the mipmapping

            tex->setMaxAnisotropy( 16.0f );

            tex->setResizeNonPowerOfTwoHint(false);
            tex->setFilter( osg::Texture::MAG_FILTER, osg::Texture::LINEAR );
            tex->setFilter( osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR );

            // configure the wrapping
            tex->setWrap( osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE );
            tex->setWrap( osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE );

            stateSet->setTextureAttributeAndModes( slot, tex, osg::StateAttribute::ON );
            
            // install the slot attribute
            std::string name = makeSamplerName(slot);
            stateSet->getOrCreateUniform( name.c_str(), osg::Uniform::SAMPLER_2D )->set( slot );
        }

        // see if we need an LOD blending secondary texture:
        int secondarySlot = layout.getSlot( layerUID, 1 );
        if ( secondarySlot >= 0 )
        {
            osg::Texture2D* parentTex = 0;

            //int parentSlot = slot + layout.getRenderOrder().size();
            std::string parentSampler = makeSamplerName( secondarySlot );
            if (parentStateSet)
            {
                parentTex = static_cast<osg::Texture2D*>(
                    parentStateSet->getTextureAttribute( slot, osg::StateAttribute::TEXTURE ) );

                if (parentTex)
                {
                    stateSet->setTextureAttributeAndModes(secondarySlot, parentTex, osg::StateAttribute::ON );
                    stateSet->getOrCreateUniform(parentSampler.c_str(),
                                                 osg::Uniform::SAMPLER_2D )->set( secondarySlot );
                }
            }

            if (!parentTex)
            {
                // Bind the main texture as the secondary texture and
                // set the scaling factors appropriately.
                stateSet->getOrCreateUniform(
                    parentSampler.c_str(), osg::Uniform::SAMPLER_2D)->set(slot);
                osg::Vec4 texCoordFactors(1.0f, 1.0f, 0.0f, 0.0f);
                osg::Uniform* uTexCoordFactor = new osg::Uniform("osgearth_texCoordFactors", texCoordFactors);
                stateSet->addUniform(uTexCoordFactor);
            }
        }
        return tex;
    }
}

//------------------------------------------------------------------------

TextureCompositorMultiTexture::TextureCompositorMultiTexture( bool useGPU, const TerrainOptions& options ) :
_lodTransitionTime( *options.lodTransitionTime() ),
_enableMipmapping( *options.enableMipmapping() ),
_useGPU( useGPU )
{
    _enableMipmappingOnUpdatedTextures = Registry::instance()->getCapabilities().supportsMipmappedTextureUpdates();
}

void
TextureCompositorMultiTexture::applyLayerUpdate(osg::StateSet*       stateSet,
                                                UID                  layerUID,
                                                const GeoImage&      preparedImage,
                                                const TileKey&       tileKey,
                                                const TextureLayout& layout,
                                                osg::StateSet*       parentStateSet) const
{
    osg::Texture2D* tex = s_getTexture( stateSet, layerUID, layout, parentStateSet);
    if ( tex )
    {
        osg::Image* image = preparedImage.getImage();
        image->dirty(); // required for ensure the texture recognizes the image as new data
        tex->setImage( image );

        // set up proper mipmapping filters:
        if (_enableMipmapping &&
            _enableMipmappingOnUpdatedTextures && 
            ImageUtils::isPowerOfTwo( image ) && 
            !(!image->isMipmap() && ImageUtils::isCompressed(image)) )
        {
            if ( tex->getFilter(osg::Texture::MIN_FILTER) != osg::Texture::LINEAR_MIPMAP_LINEAR )
                tex->setFilter( osg::Texture::MIN_FILTER, osg::Texture::LINEAR_MIPMAP_LINEAR );
        }
        else if ( tex->getFilter(osg::Texture::MIN_FILTER) != osg::Texture::LINEAR )
        {
            tex->setFilter( osg::Texture::MIN_FILTER, osg::Texture::LINEAR );
        }

        bool lodBlending = layout.getSlot(layerUID, 1) >= 0;

        if (_enableMipmapping &&
            _enableMipmappingOnUpdatedTextures && 
            lodBlending )
        {
            // update the timestamp on the image layer to support blending.
            osg::ref_ptr<ArrayUniform> stamp = new ArrayUniform( stateSet, "osgearth_SlotStamp" );
            if ( !stamp->isComplete() || stamp->getNumElements() < layout.getMaxUsedSlot() + 1 )
            {
                stamp = new ArrayUniform( osg::Uniform::FLOAT, "osgearth_SlotStamp", layout.getMaxUsedSlot()+1 );
                stamp->addTo( stateSet );
            }

            float now = (float)osg::Timer::instance()->delta_s( osg::Timer::instance()->getStartTick(), osg::Timer::instance()->tick() );
            stamp->setElement( layout.getSlot(layerUID, 0), now );
        }
    }
}

void 
TextureCompositorMultiTexture::updateMasterStateSet(osg::StateSet*       stateSet,
                                                    const TextureLayout& layout    ) const
{
    int numSlots = layout.getMaxUsedSlot() + 1;
    int maxUnits = numSlots;
//    if (_useGPU && _lodBlending)
//        maxUnits = numSlots * 2;
    if ( _useGPU )
    {
        // Validate against the max number of GPU texture units:
        if ( maxUnits > Registry::instance()->getCapabilities().getMaxGPUTextureUnits() )
        {
            maxUnits = Registry::instance()->getCapabilities().getMaxGPUTextureUnits();
            //numSlots = maxUnits / 2;
            OE_WARN << LC
                << "Warning! You have exceeded the number of texture units available on your GPU ("
                << maxUnits << "). Consider using another compositing mode."
                << std::endl;
        }

        VirtualProgram* vp = static_cast<VirtualProgram*>( stateSet->getAttribute(osg::StateAttribute::PROGRAM) );
        if ( maxUnits > 0 )
        {
            // see if we have any blended layers:
            bool hasBlending = layout.containsSecondarySlots( maxUnits ); 

            vp->setShader( 
                "osgearth_vert_setupTexturing", 
                s_createTextureVertexShader(layout, hasBlending) ); //_lodBlending) );

            vp->setShader( 
                "osgearth_frag_applyTexturing",
                s_createTextureFragShaderFunction(layout, maxUnits, hasBlending, _lodTransitionTime ) );
        }
        else
        {
            vp->removeShader( "osgearth_frag_applyTexturing", osg::Shader::FRAGMENT );
            vp->removeShader( "osgearth_vert_setupTexturing", osg::Shader::VERTEX );
        }
    }

    else
    {
        // Validate against the maximum number of textures available in FFP mode.
        if ( maxUnits > Registry::instance()->getCapabilities().getMaxFFPTextureUnits() )
        {
            maxUnits = Registry::instance()->getCapabilities().getMaxFFPTextureUnits();
            OE_WARN << LC << 
                "Warning! You have exceeded the number of texture units available in fixed-function pipeline "
                "mode on your graphics hardware (" << maxUnits << "). Consider using another "
                "compositing mode." << std::endl;
        }

        // FFP multitexturing requires that we set up a series of TexCombine attributes:
        if (maxUnits == 1)
        {
            osg::TexEnv* texenv = new osg::TexEnv(osg::TexEnv::MODULATE);
            stateSet->setTextureAttributeAndModes(0, texenv, osg::StateAttribute::ON);
        }
        else if (maxUnits >= 2)
        {
            //Blend together the colors and accumulate the alpha values of textures 0 and 1 on unit 0
            {
                osg::TexEnvCombine* texenv = new osg::TexEnvCombine;
                texenv->setCombine_RGB(osg::TexEnvCombine::INTERPOLATE);
                texenv->setCombine_Alpha(osg::TexEnvCombine::ADD);

                texenv->setSource0_RGB(osg::TexEnvCombine::TEXTURE0+1);
                texenv->setOperand0_RGB(osg::TexEnvCombine::SRC_COLOR);
                texenv->setSource0_Alpha(osg::TexEnvCombine::TEXTURE0+1);
                texenv->setOperand0_Alpha(osg::TexEnvCombine::SRC_ALPHA);

                texenv->setSource1_RGB(osg::TexEnvCombine::TEXTURE0+0);
                texenv->setOperand1_RGB(osg::TexEnvCombine::SRC_COLOR);
                texenv->setSource1_Alpha(osg::TexEnvCombine::TEXTURE0+0);
                texenv->setOperand1_Alpha(osg::TexEnvCombine::SRC_ALPHA);

                texenv->setSource2_RGB(osg::TexEnvCombine::TEXTURE0+1);
                texenv->setOperand2_RGB(osg::TexEnvCombine::SRC_ALPHA);

                stateSet->setTextureAttributeAndModes(0, texenv, osg::StateAttribute::ON);
            }


            //For textures 2 and beyond, blend them together with the previous
            //Add the alpha values of this unit and the previous unit
            for (int unit = 1; unit < maxUnits-1; ++unit)
            {
                osg::TexEnvCombine* texenv = new osg::TexEnvCombine;
                texenv->setCombine_RGB(osg::TexEnvCombine::INTERPOLATE);
                texenv->setCombine_Alpha(osg::TexEnvCombine::ADD);

                texenv->setSource0_RGB(osg::TexEnvCombine::TEXTURE0+unit+1);
                texenv->setOperand0_RGB(osg::TexEnvCombine::SRC_COLOR);
                texenv->setSource0_Alpha(osg::TexEnvCombine::TEXTURE0+unit+1);
                texenv->setOperand0_Alpha(osg::TexEnvCombine::SRC_ALPHA);

                texenv->setSource1_RGB(osg::TexEnvCombine::PREVIOUS);
                texenv->setOperand1_RGB(osg::TexEnvCombine::SRC_COLOR);
                texenv->setSource1_Alpha(osg::TexEnvCombine::PREVIOUS);
                texenv->setOperand1_Alpha(osg::TexEnvCombine::SRC_ALPHA);

                texenv->setSource2_RGB(osg::TexEnvCombine::TEXTURE0+unit+1);
                texenv->setOperand2_RGB(osg::TexEnvCombine::SRC_ALPHA);

                stateSet->setTextureAttributeAndModes(unit, texenv, osg::StateAttribute::ON);
            }

            //Modulate the colors to get proper lighting on the last unit
            //Keep the alpha results from the previous stage
            {
                osg::TexEnvCombine* texenv = new osg::TexEnvCombine;
                texenv->setCombine_RGB(osg::TexEnvCombine::MODULATE);
                texenv->setCombine_Alpha(osg::TexEnvCombine::REPLACE);

                texenv->setSource0_RGB(osg::TexEnvCombine::PREVIOUS);
                texenv->setOperand0_RGB(osg::TexEnvCombine::SRC_COLOR);
                texenv->setSource0_Alpha(osg::TexEnvCombine::PREVIOUS);
                texenv->setOperand0_Alpha(osg::TexEnvCombine::SRC_ALPHA);

                texenv->setSource1_RGB(osg::TexEnvCombine::PRIMARY_COLOR);
                texenv->setOperand1_RGB(osg::TexEnvCombine::SRC_COLOR);
                stateSet->setTextureAttributeAndModes(maxUnits-1, texenv, osg::StateAttribute::ON);
            }
        }
    }
}

osg::Shader*
TextureCompositorMultiTexture::createSamplerFunction(UID layerUID,
                                                     const std::string& functionName,
                                                     osg::Shader::Type type,
                                                     const TextureLayout& layout ) const
{
    osg::Shader* result = 0L;

    int slot = layout.getSlot( layerUID );
    if ( slot >= 0 )
    {
        std::stringstream buf;

        buf << "uniform sampler2D tex"<< slot << "; \n"
            << "vec4 " << functionName << "() \n"
            << "{ \n";

        if ( type == osg::Shader::VERTEX )
            buf << "    return texture2D(tex"<< slot << ", gl_MultiTexCoord"<< slot <<".st); \n";
        else
            buf << "    return texture2D(tex"<< slot << ", gl_TexCoord["<< slot << "].st); \n";

        buf << "} \n";

        std::string str = buf.str();
        result = new osg::Shader( type, str );
    }
    return result;
}
