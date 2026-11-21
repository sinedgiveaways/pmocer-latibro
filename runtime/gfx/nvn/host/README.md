# runtime/gfx/nvn/host

The host graphics layer: its interface and the null back end, present and window, the programs' prepared SPIR-V and
the texts the host shows.

| File | What it is |
|---|---|
| `gfx.h`, `gfx_null.c` | Graphics layer interface, null back end (no GPU) and `gfx_backend`, which picks the back end and wraps it with the capture of `../cap/` (`ORBITALS_CAPTURE`) |
| `gfx_present.c`, `gfx_wsys.h`, `gfx_window_sdl.c` | Swapchain, present and vsync; SDL3 window |
| `gfx_hdr.c` | HDR output (`ORBITALS_HDR` auto/off): what nn::vi tells the game about the display's HDR (`ORBITALS_HDR_FAKE`), the HDR10 swapchain format and metadata, and 10-bit PQ pixels to sRGB bytes for shots, captures and thumbnails |
| `gfx_shader.c` | Loads MKW's prepared SPIR-V (`prepared_get`, key `nvn2_program_key`), variants with integer inputs, integer outputs or integer images and reflection (`.res.json` and the SPIR-V itself); loads the pipeline prewarm shaders in parallel (`gfx_shader_prewarm`) |
| `gfx_shader_auto.c` | Translates the programs a run dumps without SPIR-V in the background (`tools/prepare/dumped_shaders.py` in a child process, a few seconds after the last dump); when it finishes, the replay looks those programs up again, so they draw in the same run (`ORBITALS_SHADERS_AUTO=0` turns it off) |
| `gfx_spirv_io.c` | The integer-inputs patch of a vertex shader and the integer-outputs patch of a fragment shader: the inputs whose attribute format, or the outputs whose colour target, is UINT/SINT become u/ivec4, their loads and stores bitcast (issue #64) |
| `gfx_spirv_img.c` | The integer-images patch of a compute program: the textures and storage images whose bound view is UINT/SINT become integer images, their reads and writes bitcast (VUID format-07753, issue #64) |
| `ui_text.c`, `ui_text.h` | Texts the host shows the player (software keyboard, menu and start screen; menu in English, Russian and Spanish) |
| `ui_text_cat.h` | What every language's catalog shares (the language names, the arrays' declarations); the catalogs themselves are generated from `i18n/*.po` |
