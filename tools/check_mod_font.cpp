#include "loader/mod_font.h"
#include <cstdio>

// Run with a Chinese font on the host using the same ImGui/FreeType sources as
// the APK.
int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  ImGui::CreateContext();
  auto &atlas = *ImGui::GetIO().Fonts;
  ModFontGlyphs glyphs(atlas);
  auto *font = atlas.AddFontFromFileTTF(argv[1], 34, nullptr, glyphs.ranges());
  if (!font || !atlas.Build())
    return 3;
  constexpr char phrase[] = "雌火龙的上等鳞";
  auto missing = [&] {
    unsigned int count = 0;
    for (const char *s = phrase; *s;) {
      unsigned int cp;
      s += ImTextCharFromUtf8(&cp, s, nullptr);
      if (!font->FindGlyphNoFallback(static_cast<ImWchar>(cp)))
        ++count;
    }
    return count;
  };
  auto before = missing();
  glyphs.observe(phrase);
  auto updated = glyphs.update(atlas, 8192);
  auto after = updated == ModFontGlyphs::Update::Failed ? before : missing();
  glyphs.observe(phrase);
  auto repeated = glyphs.update(atlas, 8192);
  std::printf(
      "Missing glyphs before=%u after=%u, repeat rebuild=%d, atlas=%dx%d\n",
      before, after, repeated != ModFontGlyphs::Update::Unchanged,
      atlas.TexWidth, atlas.TexHeight);
  ImGui::DestroyContext();
  return before && !after && updated == ModFontGlyphs::Update::Built &&
                 repeated == ModFontGlyphs::Update::Unchanged
             ? 0
             : 1;
}
