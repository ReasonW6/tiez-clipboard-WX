// Offscreen integration of actual DOM geometry with the production shader.
// The fixture is fixed at 425 x 589 CSS pixels and deviceScaleFactor 1.
#define TIEZ_GPU_WIDTH 425
#define TIEZ_GPU_HEIGHT 589
#define wmain isolated_math_regression_entrypoint
#include "glass_gpu_regression.cpp"
#undef wmain
#include "glass_prefix_gpu.h"

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 3) throw std::runtime_error("Expected production shader and fixture directory");
        PrefixGpu gpu(argv[1]);
        unsigned rendered = 0;
        for (const auto& file : std::filesystem::directory_iterator(argv[2])) {
            if (file.path().extension() != L".state") continue;
            State state;
            std::ifstream parameters(file.path(), std::ios::binary);
            if (!parameters.read(reinterpret_cast<char*>(&state), sizeof(state)) || parameters.peek() != EOF)
                throw std::runtime_error("Incorrect state length");
            if (state.size[0] != Width || state.size[1] != Height || state.options[3] != 1
                || state.options[1] < 0 || state.options[1] > 64 || state.trackMeta[0] > 64)
                throw std::runtime_error("Unexpected fixture dimensions or control budget");
            auto inputPath = file.path(); inputPath.replace_extension(L".rgba");
            std::ifstream source(inputPath, std::ios::binary);
            Pixels pixels; pixels.reserve(Width * Height);
            for (unsigned i = 0; i < Width * Height; ++i) {
                std::array<uint8_t, 4> value{};
                if (!source.read(reinterpret_cast<char*>(value.data()), value.size()))
                    throw std::runtime_error("Incomplete input pixels");
                pixels.push_back({value[0]/255.f, value[1]/255.f, value[2]/255.f, value[3]/255.f});
            }
            if (source.peek() != EOF) throw std::runtime_error("Extra input pixels");
            auto result = gpu.material(pixels, state, true);
            auto outputPath = file.path(); outputPath.replace_extension(L".glass.rgba");
            std::ofstream output(outputPath, std::ios::binary);
            for (auto& pixel : result) for (float channel : pixel) {
                uint8_t value = uint8_t(std::lround(std::clamp(channel, 0.f, 1.f) * 255));
                output.write(reinterpret_cast<const char*>(&value), 1);
            }
            if (!output) throw std::runtime_error("Could not write output pixels");
            std::cout << file.path().stem().string() << ": " << state.options[1] << " controls\n";
            ++rendered;
        }
        if (!rendered) throw std::runtime_error("No exported layouts");
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
