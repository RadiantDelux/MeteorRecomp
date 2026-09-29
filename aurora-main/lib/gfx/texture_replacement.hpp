#pragma once

#include "texture.hpp"
#include <optional>
#include <string_view>

namespace aurora::gfx::texture_replacement {
void initialize() noexcept;
void shutdown() noexcept;
void register_tlut(const GXTlutObj* obj, const void* data, GXTlutFmt format, uint16_t entries) noexcept;
void load_tlut(const GXTlutObj* obj, uint32_t idx) noexcept;
std::optional<TextureHandle> find_replacement(const GXTexObj_& obj, const GXTlutObj_* tlut = nullptr) noexcept;
std::string build_texture_replacement_name(const GXTexObj_& obj) noexcept;
bool set_controller_prompt_style(std::string_view style) noexcept;
} // namespace aurora::gfx::texture_replacement
