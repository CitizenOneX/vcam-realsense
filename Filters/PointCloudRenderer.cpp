#include "PointCloudRenderer.h"
#include "vs-pointcloud.h"
#include "ps-pointcloud.h"

#include <d3dcompiler.h>    // shader compiler
#include <DirectXMath.h>    // matrix/vector math
#include <cassert>

struct VertexPositionTexUv {
    DirectX::XMFLOAT3 Pos;
    DirectX::XMFLOAT2 TexUv;
};

struct VS_CONSTANT_BUFFER
{
    DirectX::XMMATRIX worldViewProj;
};


PointCloudRenderer::PointCloudRenderer() : m_InputDepthWidth(0), m_InputDepthHeight(0), m_InputTexWidth(0), m_InputTexHeight(0), m_OutputWidth(0), m_OutputHeight(0), m_ClippingDistanceZ(1.3f)
{
}

PointCloudRenderer::~PointCloudRenderer()
{
}

HRESULT PointCloudRenderer::Init(int inputDepthWidth, int inputDepthHeight, int inputTexWidth, int inputTexHeight, int outputWidth, int outputHeight, float clippingDistanceZ)
{
    m_InputDepthWidth = inputDepthWidth;
    m_InputDepthHeight = inputDepthHeight;
    m_InputTexWidth = inputTexWidth;
    m_InputTexHeight = inputTexHeight;
    m_OutputWidth = outputWidth;
    m_OutputHeight = outputHeight;
    m_ClippingDistanceZ = clippingDistanceZ;

    // Set up Direct3D Device and Device Context
    {
        {
            D3D_FEATURE_LEVEL feature_level;
            UINT flags = D3D11_CREATE_DEVICE_SINGLETHREADED;
#if defined( DEBUG ) || defined( _DEBUG )
            flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
            HRESULT hr = D3D11CreateDevice(
                NULL,
                D3D_DRIVER_TYPE_HARDWARE,
                NULL,
                flags,
                NULL,
                0,
                D3D11_SDK_VERSION,
                &device_ptr,
                &feature_level,
                &device_context_ptr);
            assert(S_OK == hr && device_ptr && device_context_ptr);
        }

        // Render Target
        {
            // Create the render target texture (which will be copied back to caller's output frame)
            D3D11_TEXTURE2D_DESC desc_target = {};
            desc_target.Width = outputWidth;
            desc_target.Height = outputHeight;
            desc_target.ArraySize = 1;
            desc_target.SampleDesc.Count = 1;
            desc_target.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // .... DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            desc_target.BindFlags = D3D11_BIND_RENDER_TARGET;
            desc_target.Usage = D3D11_USAGE_DEFAULT;
            HRESULT hr = device_ptr->CreateTexture2D(&desc_target, nullptr, &target_ptr);
            assert(SUCCEEDED(hr));
        }

        // depth stencil
        {
            // Create the depth stencil for the render target
            D3D11_TEXTURE2D_DESC desc_depth;
            desc_depth.Width = outputWidth;
            desc_depth.Height = outputHeight;
            desc_depth.MipLevels = 1;
            desc_depth.ArraySize = 1;
            desc_depth.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
            desc_depth.SampleDesc.Count = 1;
            desc_depth.SampleDesc.Quality = 0;
            desc_depth.Usage = D3D11_USAGE_DEFAULT;
            desc_depth.BindFlags = D3D11_BIND_DEPTH_STENCIL;
            desc_depth.CPUAccessFlags = 0;
            desc_depth.MiscFlags = 0;
            HRESULT hr = device_ptr->CreateTexture2D(&desc_depth, NULL, &depth_stencil_ptr);
            assert(SUCCEEDED(hr));

            D3D11_DEPTH_STENCIL_DESC depth_stencil_desc;

            // Depth test parameters
            depth_stencil_desc.DepthEnable = true;
            depth_stencil_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
            depth_stencil_desc.DepthFunc = D3D11_COMPARISON_LESS;

            // Stencil test parameters
            depth_stencil_desc.StencilEnable = true;
            depth_stencil_desc.StencilReadMask = 0xFF;
            depth_stencil_desc.StencilWriteMask = 0xFF;

            // Stencil operations if pixel is front-facing
            depth_stencil_desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
            depth_stencil_desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_INCR;
            depth_stencil_desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
            depth_stencil_desc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;

            // Stencil operations if pixel is back-facing
            depth_stencil_desc.BackFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
            depth_stencil_desc.BackFace.StencilDepthFailOp = D3D11_STENCIL_OP_DECR;
            depth_stencil_desc.BackFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
            depth_stencil_desc.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;

            // Create depth stencil state
            hr = device_ptr->CreateDepthStencilState(&depth_stencil_desc, &depth_stencil_state_ptr);
            assert(SUCCEEDED(hr));

            // Create the depth stencil view
            D3D11_DEPTH_STENCIL_VIEW_DESC depth_stencil_view_desc;
            depth_stencil_view_desc.Format = desc_depth.Format;
            depth_stencil_view_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            depth_stencil_view_desc.Texture2D.MipSlice = 0;
            depth_stencil_view_desc.Flags = 0;

            hr = device_ptr->CreateDepthStencilView(depth_stencil_ptr, // Depth stencil texture
                &depth_stencil_view_desc, // Depth stencil desc
                &depth_stencil_view_ptr);  // [out] Depth stencil view
            assert(SUCCEEDED(hr));
        }

        // Create the Staging texture, we resource-copy GPU->GPU from target to staging, then read from staging
        // at our leisure
        {
            D3D11_TEXTURE2D_DESC desc_staging = {};
            desc_staging.Width = outputWidth;
            desc_staging.Height = outputHeight;
            desc_staging.ArraySize = 1;
            desc_staging.SampleDesc.Count = 1;
            desc_staging.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // .... DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            desc_staging.BindFlags = 0;
            desc_staging.Usage = D3D11_USAGE_STAGING;
            desc_staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            HRESULT hr = device_ptr->CreateTexture2D(&desc_staging, nullptr, &staging_ptr);
            assert(SUCCEEDED(hr));

            // create and set the render target view
            hr = device_ptr->CreateRenderTargetView(target_ptr, nullptr, &render_target_view_ptr);
            assert(SUCCEEDED(hr));
        }
    }

    // Compile the Shaders
    // NB. the shaders are compiled from HLSL files into .h header files at build time
    // otherwise at runtime the .hlsl or .cso files need to be located from the current
    // directory which can be a pain
    {
        // COMPILE VERTEX SHADER
        HRESULT hr = device_ptr->CreateVertexShader(g_vertex_shader, sizeof(g_vertex_shader) / sizeof(BYTE), nullptr, &vertex_shader_ptr);
        assert(SUCCEEDED(hr));

        // COMPILE PIXEL SHADER
        hr = device_ptr->CreatePixelShader(g_pixel_shader, sizeof(g_pixel_shader) / sizeof(BYTE), nullptr, &pixel_shader_ptr);
        assert(SUCCEEDED(hr));

        // set up input layout for vertex shader
        D3D11_INPUT_ELEMENT_DESC inputElementDesc[] = {
            // POS comes in input slot 0 (vertex position buffer)
            { "SV_POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            /*
            { "COL", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "NOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            */
        };
        hr = device_ptr->CreateInputLayout(
            inputElementDesc,
            ARRAYSIZE(inputElementDesc),
            g_vertex_shader,
            sizeof(g_vertex_shader) / sizeof(BYTE),
            &input_layout_ptr);
        assert(SUCCEEDED(hr));
    }

    // Create dynamic vertex buffer - sized to input width x height
    {
        int arrayElementCount = m_InputDepthWidth * m_InputDepthHeight;
        VertexPositionTexUv* vertex_data_array = new VertexPositionTexUv[arrayElementCount];
        ZeroMemory(vertex_data_array, arrayElementCount * sizeof(VertexPositionTexUv));

        // create vertex buffer to store the vertex data
        D3D11_BUFFER_DESC vertex_buff_descr = {};
        vertex_buff_descr.ByteWidth = arrayElementCount * sizeof(VertexPositionTexUv);
        vertex_buff_descr.Usage = D3D11_USAGE_DYNAMIC;
        vertex_buff_descr.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        vertex_buff_descr.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        D3D11_SUBRESOURCE_DATA sr_data = { 0 };
        sr_data.pSysMem = vertex_data_array;
        HRESULT hr = device_ptr->CreateBuffer(&vertex_buff_descr, &sr_data, &vertex_buffer_ptr);
        assert(SUCCEEDED(hr));
    }

    // constant buffer for world view projection matrix 
    {
        VS_CONSTANT_BUFFER VsConstData = {};

        // Set up WVP matrix, camera details
        world = DirectX::XMMatrixIdentity(); // no reflection
        //DirectX::XMMATRIX world = {-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; // reflect about x ("mirror mode")

        eyePos = DirectX::XMVectorSet(0.0f, 0.0f, 0.0f, 0.0f);
        lookAtPos = DirectX::XMVectorSet(0.0f, 0.0f, 0.5f, 0.0f); //Look at center of the world
        upVector = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f); //Positive Y Axis = Up
        view = DirectX::XMMatrixLookAtLH(eyePos, lookAtPos, upVector);

        float fovRadians = DirectX::XM_PI / 3.0f; // 60 degree FOV
        float aspectRatio = static_cast<float>(m_OutputWidth) / static_cast<float>(m_OutputHeight);
        float nearZ = 0.1f;
        float farZ = 20.0f;
        projection = DirectX::XMMatrixPerspectiveFovLH(fovRadians, aspectRatio, nearZ, farZ);

        VsConstData.worldViewProj = DirectX::XMMatrixTranspose(world * view * projection);

        // create the constant buffer descriptor
        D3D11_BUFFER_DESC constant_buff_descr;
        constant_buff_descr.ByteWidth = sizeof(VS_CONSTANT_BUFFER);
        constant_buff_descr.Usage = D3D11_USAGE_DEFAULT;
        constant_buff_descr.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        constant_buff_descr.CPUAccessFlags = 0;
        constant_buff_descr.MiscFlags = 0;
        constant_buff_descr.StructureByteStride = 0;

        // Fill in the subresource data
        D3D11_SUBRESOURCE_DATA sr_data;
        sr_data.pSysMem = &VsConstData;
        sr_data.SysMemPitch = 0;
        sr_data.SysMemSlicePitch = 0;

        // Create the buffer
        HRESULT hr = device_ptr->CreateBuffer(
            &constant_buff_descr,
            &sr_data,
            &constant_buffer_ptr);
        assert(SUCCEEDED(hr));

        // Set the constant buffer
        device_context_ptr->VSSetConstantBuffers(0, 1, &constant_buffer_ptr);
    }

    // Create the Color Texture2D updated each frame with RGB/IR camera and matching SamplerState
    {
        D3D11_TEXTURE2D_DESC texDesc;
        texDesc.Width = m_InputTexWidth;
        texDesc.Height = m_InputTexHeight;
        texDesc.MipLevels = texDesc.ArraySize = 1;
        texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.SampleDesc.Quality = 0;
        texDesc.Usage = D3D11_USAGE_DYNAMIC;
        texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        texDesc.MiscFlags = 0;

        HRESULT hr = device_ptr->CreateTexture2D(&texDesc, NULL, &color_tex_ptr);
        assert(SUCCEEDED(hr));

        // Create a texture sampler state description.
        D3D11_SAMPLER_DESC samplerDesc;
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.MipLODBias = 0.0f;
        samplerDesc.MaxAnisotropy = 1;
        samplerDesc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
        samplerDesc.BorderColor[0] = 0;
        samplerDesc.BorderColor[1] = 0;
        samplerDesc.BorderColor[2] = 0;
        samplerDesc.BorderColor[3] = 0;
        samplerDesc.MinLOD = 0;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

        // Create the texture sampler state.
        hr = device_ptr->CreateSamplerState(&samplerDesc, &sampler_state_ptr);
        assert(SUCCEEDED(hr));

        // Setup the shader resource view description.
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
        srvDesc.Format = texDesc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = -1;

        // Create the shader resource view for the texture.
        hr = device_ptr->CreateShaderResourceView(color_tex_ptr, &srvDesc, &tex_view_ptr);
        assert(SUCCEEDED(hr));
    }

    // set background color for point clouds
#if defined( DEBUG ) || defined( _DEBUG )
    // set the default background color to quarter cornflower blue for Debug builds
    m_BackgroundColor = new float[] { 0x64 / 255.0f / 4.0f, 0x95 / 255.0f / 4.0f, 0xED / 255.0f / 4.0f, 1.0f };
#else
    // set the default background color to black for Release builds
    m_BackgroundColor = new float[] { 0.0f, 0.0f, 0.0f, 1.0f };
#endif // DEBUG

    // set the viewport and other rendering settings that never change(!)
    {
        D3D11_VIEWPORT viewport = { 0.0f, 0.0f, static_cast<float>(m_OutputWidth), static_cast<float>(m_OutputHeight), 0.0f, 1.0f };
        device_context_ptr->RSSetViewports(1, &viewport);

        // set the output merger
        device_context_ptr->OMSetDepthStencilState(depth_stencil_state_ptr, 1);
        device_context_ptr->OMSetRenderTargets(1, &render_target_view_ptr, depth_stencil_view_ptr);

        // set the input assembler
        UINT vertex_stride = sizeof(VertexPositionTexUv);
        UINT vertex_offset = 0;
        device_context_ptr->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
        device_context_ptr->IASetInputLayout(input_layout_ptr);
        device_context_ptr->IASetVertexBuffers(0, 1, &vertex_buffer_ptr, &vertex_stride, &vertex_offset);

        // set the texture and the sampler
        device_context_ptr->PSSetShaderResources(0, 1, &tex_view_ptr);
        device_context_ptr->PSSetSamplers(0, 1, &sampler_state_ptr);

        // set the shaders
        device_context_ptr->VSSetShader(vertex_shader_ptr, NULL, 0);
        device_context_ptr->PSSetShader(pixel_shader_ptr, NULL, 0);

    }

    return S_OK;;
}

void PointCloudRenderer::UnInit()
{
    if (m_BackgroundColor) delete m_BackgroundColor;
    if (tex_view_ptr) tex_view_ptr->Release();
    if (depth_stencil_view_ptr) depth_stencil_view_ptr->Release();
    if (depth_stencil_state_ptr) depth_stencil_state_ptr->Release();
    if (depth_stencil_ptr) depth_stencil_ptr->Release();
    if (sampler_state_ptr) sampler_state_ptr->Release();
    if (color_tex_ptr) color_tex_ptr->Release();
    if (render_target_view_ptr) render_target_view_ptr->Release();
    if (input_layout_ptr) input_layout_ptr->Release();
    if (constant_buffer_ptr) constant_buffer_ptr->Release();
    if (vertex_shader_ptr) vertex_shader_ptr->Release();
    if (pixel_shader_ptr) pixel_shader_ptr->Release();
    if (vertex_buffer_ptr) vertex_buffer_ptr->Release();
    if (staging_ptr) staging_ptr->Release();
    if (target_ptr) target_ptr->Release();
    if (device_context_ptr) device_context_ptr->Release();
    if (device_ptr) device_ptr->Release();
}

void PointCloudRenderer::RenderFrame(BYTE* outputFrameBuffer, const int outputFrameLength, const unsigned int pointsCount, const float* pointsXyz, const float* texUvs, const void* color_frame_data, const int color_frame_size)
{
    assert(outputFrameBuffer != NULL && pointsXyz != NULL && (pointsCount == m_InputDepthWidth * m_InputDepthHeight) && (outputFrameLength == m_OutputWidth * m_OutputHeight * 3));

    // upload the color texture
    {
        D3D11_MAPPED_SUBRESOURCE mappedResource = { 0 };

        //  Disable GPU access to the texture data.
        HRESULT hr = device_context_ptr->Map(color_tex_ptr, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
        assert(SUCCEEDED(hr));

        //  Copy over the texture data here.
        if (color_frame_size == 0)
        {
            // Point cloud, no IR or Color frame, set the texture to opaque white
            memset(mappedResource.pData, 255, (size_t)4 * pointsCount);
        }
        else if (color_frame_size == pointsCount) 
        {
            // IR frame: copy Y8 value over to RGB (and set A to 255)
            BYTE* data = ((BYTE*)mappedResource.pData);
            BYTE* colorFrame = (BYTE*)color_frame_data;
            for (unsigned int i = 0; i < pointsCount; i++)
            {
                data[4 * i] = colorFrame[i];
                data[4 * i + 1] = colorFrame[i];
                data[4 * i + 2] = colorFrame[i];
                data[4 * i + 3] = 255;
            }
        }
        else
        {
            // RGBA color frame
            memcpy(mappedResource.pData, color_frame_data, color_frame_size);
        }

        //  Reenable GPU access to the texture data.
        device_context_ptr->Unmap(color_tex_ptr, 0);
    }

    // copy/set/map the updated vertex position data into the vertex position buffer
    unsigned int currPoint = 0; // track valid points (exclude distant points)
    {
        D3D11_MAPPED_SUBRESOURCE mappedResource = { 0 };

        //  Disable GPU access to the vertex buffer data.
        HRESULT hr = device_context_ptr->Map(vertex_buffer_ptr, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
        assert(SUCCEEDED(hr));

        //  Update the vertex buffer here.
        float* data = ((float*)mappedResource.pData);
        for (unsigned int p = 0; p < pointsCount; p++)
        {
            // only include points less than m_ClippingDistanceZ
            if (pointsXyz[3 * p + 2] < m_ClippingDistanceZ)
            {
                // lay out the points (3 floats) then the tex uvs (2 floats) in the vertex buffer struct
                // TODO do I need to align these to 16-bytes with filler?
                data[5 * currPoint] = pointsXyz[3 * p];
                data[5 * currPoint + 1] = pointsXyz[3 * p + 1];
                data[5 * currPoint + 2] = pointsXyz[3 * p + 2];
                data[5 * currPoint + 3] = texUvs[2 * p];
                data[5 * currPoint + 4] = texUvs[2 * p + 1];
                currPoint++;
            }
        }
        mappedResource.pData = data;

        //  Reenable GPU access to the vertex buffer data.
        device_context_ptr->Unmap(vertex_buffer_ptr, 0);
    }

    // update the camera position with a bit of drift
    {
        // Update our time
        static float t = 0.0f;
        {
            static ULONGLONG timeStart = 0;
            ULONGLONG timeCur = GetTickCount64();
            if (timeStart == 0)
                timeStart = timeCur;
            t = (timeCur - timeStart) / 1000.0f;
        }

        // TODO UpdateSubresource (with DEFAULT buffer usage) works smoothly straight away,
        // but the Map/Unmap approach with DYNAMIC buffer usage resulted in choppy performance.
        // This is the opposite of what's suggested by the documentation from what I can tell.

        // move the camera eyePos around in a dizzying circle (just a demo!)
        // calculate and copy the updated wvp matrix
        VS_CONSTANT_BUFFER VsConstData = {};
        eyePos = DirectX::XMVectorSet(DirectX::XMScalarSinEst(t/2.0f) / 5.0f, -0.2f + DirectX::XMScalarCosEst(t/2.0f) / 5.0f, 0.0f, 0.0f); // FIXME rotate an amount based on a time interval!
        view = DirectX::XMMatrixLookAtLH(eyePos, lookAtPos, upVector);
        VsConstData.worldViewProj = DirectX::XMMatrixTranspose(world * view * projection);
        device_context_ptr->UpdateSubresource(constant_buffer_ptr, 0, nullptr, &VsConstData, 0, 0);
    }

    // clear to the background color
    device_context_ptr->ClearRenderTargetView(render_target_view_ptr, m_BackgroundColor);
    device_context_ptr->ClearDepthStencilView(depth_stencil_view_ptr, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

    // draw the points
    device_context_ptr->Draw(currPoint, 0); // currPoint now holds the total count of valid vertices

    // flush the DirectX to the render target
    device_context_ptr->Flush();

    // Duplicate render target texture to the staging texture so we can get at it from the CPU
    device_context_ptr->CopyResource(staging_ptr, target_ptr);
    
    // Map/memcpy/Unmap the staging data to main memory
    {
        D3D11_MAPPED_SUBRESOURCE mappedResource;
        HRESULT hr = device_context_ptr->Map(staging_ptr, 0, D3D11_MAP_READ, 0, &mappedResource);
        assert(SUCCEEDED(hr));
        // pData is 32bit, outputFrameBuffer is 24bit, and one of them is BGR I think?
        convert32bppToRGB(outputFrameBuffer, outputFrameLength, (BYTE*)mappedResource.pData, m_OutputWidth * m_OutputHeight);
        device_context_ptr->Unmap(staging_ptr, 0);
    }
}

/// <summary>
/// assuming the 32bits per pixel is an RGBA value
/// then replicate it in the R, G and B bytes of the output frame buffer.
/// Also flip the bytes around to(from?) BGR.
/// </summary>
/// <param name="frameBuffer">output buffer, 24bpp</param>
/// <param name="frameSize">output buffer size in bytes</param>
/// <param name="pData">32bpp RGBA render target from Direct3D</param>
/// <param name="pixelCount">The number of pixels in the output images (same for both)</param>
void PointCloudRenderer::convert32bppToRGB(BYTE* frameBuffer, int frameSize, BYTE* pData, int pixelCount)
{
    for (int i = 0; i < pixelCount; ++i)
    {
        frameBuffer[3 * i] = pData[4 * i + 2];
        frameBuffer[3 * i + 1] = pData[4 * i + 1];
        frameBuffer[3 * i + 2] = pData[4 * i + 0];
    }
}