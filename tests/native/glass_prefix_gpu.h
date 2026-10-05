#pragma once
// Production prefix entry points exercised offscreen; no HWND or desktop read.
struct PrefixGpu : Gpu {
    ComPtr<ID3D11PixelShader> init, glass, track, finish;
    PrefixGpu(const std::filesystem::path& source, D3D_DRIVER_TYPE driver = D3D_DRIVER_TYPE_WARP) : Gpu(source, driver) {
        for (auto item : {std::pair{"PrefixInitPS", init.GetAddressOf()}, {"PrefixGlassPS", glass.GetAddressOf()},
                {"PrefixTrackPS", track.GetAddressOf()}, {"PrefixFinishPS", finish.GetAddressOf()}}) {
            ComPtr<ID3DBlob> code, error;
            auto result = D3DCompileFromFile(source.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                item.first, "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
                0, &code, &error);
            if (FAILED(result) && error) std::cerr << static_cast<const char*>(error->GetBufferPointer());
            require(result, item.first);
            require(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, item.second), item.first);
        }
    }
    Texture mip(const Texture& source) {
        D3D11_TEXTURE2D_DESC d{}; source.texture->GetDesc(&d);
        auto result = auxiliary(d.Format, true);
        context->CopySubresourceRegion(result.texture.Get(), 0, 0, 0, 0, source.texture.Get(), 0, nullptr);
        context->GenerateMips(result.source.Get()); return result;
    }
    void pair(const Texture& input, const Texture* inputMask, Texture& output, Texture& outputMask,
        State state, ID3D11PixelShader* shader, bool generateMips = true) {
        context->UpdateSubresource(constants.Get(), 0, nullptr, &state, 0, 0);
        D3D11_VIEWPORT viewport{0, 0, float(Width), float(Height), 0, 1}; context->RSSetViewports(1, &viewport);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertex.Get(), nullptr, 0); context->PSSetShader(shader, nullptr, 0);
        auto cb = constants.Get(); auto ss = sampler.Get();
        context->PSSetConstantBuffers(0, 1, &cb); context->PSSetSamplers(0, 1, &ss);
        ID3D11ShaderResourceView* srvs[]{input.source.Get(), nullptr, inputMask ? inputMask->source.Get() : nullptr};
        ID3D11RenderTargetView* rtvs[]{output.target.Get(), outputMask.target.Get()};
        context->PSSetShaderResources(0, 3, srvs); context->OMSetRenderTargets(2, rtvs, nullptr); context->Draw(3, 0);
        ID3D11ShaderResourceView* none[3]{}; context->PSSetShaderResources(0, 3, none);
        context->OMSetRenderTargets(0, nullptr, nullptr); if (generateMips) context->GenerateMips(output.source.Get());
    }
    Pixels material(const Pixels& pixels, State state, bool prefix) {
        auto raw = texture(&pixels), horizontal = auxiliary(DXGI_FORMAT_R16G16B16A16_FLOAT), base = texture(), output = texture();
        state.direction[0] = 1; state.direction[1] = 0; state.options[2] = 0;
        draw(raw, horizontal, state, blur.Get());
        state.direction[0] = 0; state.direction[1] = 1; state.options[2] = 1;
        draw(horizontal, base, state, blur.Get());
        if (!prefix) {
            auto pyramid = mip(raw); auto view = pyramid.source.Get(); context->PSSetShaderResources(1, 1, &view);
            draw(base, output, state, optics.Get()); ID3D11ShaderResourceView* none{}; context->PSSetShaderResources(1, 1, &none);
            return read(output);
        }
        return compositePrefix(raw, base, state);
    }
    Pixels compositePrefix(const Texture& raw, const Texture& base, State state, bool mipEnabled = true) {
        auto output = texture();
        auto initial = auxiliary(DXGI_FORMAT_R16G16B16A16_FLOAT, mipEnabled);
        Texture colors[]{auxiliary(DXGI_FORMAT_R16G16B16A16_FLOAT, mipEnabled), auxiliary(DXGI_FORMAT_R16G16B16A16_FLOAT, mipEnabled)};
        Texture masks[]{auxiliary(DXGI_FORMAT_R16_FLOAT), auxiliary(DXGI_FORMAT_R16_FLOAT)};
        pair(raw, nullptr, initial, masks[0], state, init.Get(), mipEnabled);
        const Texture* current = &initial; unsigned index = 0;
        auto node = [&](State parameters, ID3D11PixelShader* shader) {
            unsigned next = 1 - index;
            pair(*current, &masks[index], colors[next], masks[next], parameters, shader, mipEnabled);
            index = next; current = &colors[next];
        };
        for (unsigned i = 0; i < unsigned(state.options[1]); ++i) {
            State parameters = state; parameters.controls[0] = state.controls[i];
            for (unsigned j = 0; j < unsigned(state.trackMeta[0]); ++j) {
                if (unsigned(state.tracks[j].shape[2]) == i) {
                    parameters.tracks[0] = state.tracks[j]; node(parameters, track.Get());
                }
            }
            node(parameters, glass.Get());
        }
        ID3D11ShaderResourceView* extra[]{initial.source.Get(), masks[index].source.Get(), base.source.Get()};
        context->PSSetShaderResources(1, 3, extra); draw(*current, output, state, finish.Get());
        ID3D11ShaderResourceView* none[3]{}; context->PSSetShaderResources(1, 3, none);
        return read(output);
    }
};

