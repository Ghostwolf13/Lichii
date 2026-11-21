// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>

#include "video_core/surface.h"
#include "video_core/textures/texture.h"

namespace VideoCommon {

/// Like PixelFormatFromTextureInfo, but returns nothing for a tuple it does not know.
std::optional<VideoCore::Surface::PixelFormat> TryPixelFormatFromTextureInfo(
    Tegra::Texture::TextureFormat format, Tegra::Texture::ComponentType red_component,
    Tegra::Texture::ComponentType green_component, Tegra::Texture::ComponentType blue_component,
    Tegra::Texture::ComponentType alpha_component, bool is_srgb) noexcept;

VideoCore::Surface::PixelFormat PixelFormatFromTextureInfo(
    Tegra::Texture::TextureFormat format, Tegra::Texture::ComponentType red_component,
    Tegra::Texture::ComponentType green_component, Tegra::Texture::ComponentType blue_component,
    Tegra::Texture::ComponentType alpha_component, bool is_srgb) noexcept;

} // namespace VideoCommon
