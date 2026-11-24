#pragma once
#include <imgui.h>
#include <imgui_internal.h>

// ImGui 1.91 bakes a fixed atlas. Keep the character set with its context and
// extend it from actual widget text between frames, on the GL thread.
class ModFontGlyphs {
  ImFontGlyphRangesBuilder wanted;
  ImVector<ImWchar> active;
  bool changed = false;

public:
  explicit ModFontGlyphs(ImFontAtlas &atlas) {
    wanted.AddRanges(atlas.GetGlyphRangesChineseSimplifiedCommon());
    wanted.BuildRanges(&active);
  }
  const ImWchar *ranges() const { return active.Data; }
  void observe(const char *text) {
    while (text && *text) {
      unsigned int cp;
      text += ImTextCharFromUtf8(&cp, text, nullptr);
      if (cp > IM_UNICODE_CODEPOINT_MAX || wanted.GetBit(cp))
        continue;
      wanted.AddChar(static_cast<ImWchar>(cp));
      changed = true;
    }
  }
  enum class Update { Unchanged, Built, Restored, Failed };
  Update update(ImFontAtlas &atlas, int max_texture_size) {
    if (!changed)
      return Update::Unchanged;
    changed = false;
    ImVector<ImWchar> next;
    wanted.BuildRanges(&next);
    atlas.Sources[0].GlyphRanges = next.Data;
    if (atlas.Build() && atlas.TexWidth <= max_texture_size &&
        atlas.TexHeight <= max_texture_size) {
      active.swap(next);
      return Update::Built;
    }
    atlas.Sources[0].GlyphRanges = active.Data;
    return atlas.Build() ? Update::Restored : Update::Failed;
  }
};
