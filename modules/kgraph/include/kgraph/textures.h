#pragma once

/**
 * @file textures.h
 * @brief How a scene's picture files are kept on the device: as they are read, or block-compressed — cooked once into
 *        the project's `.koral/cache/textures` and read from there from then on — and pictures made of the channels of others.
 *
 * Cooking changes nothing a scene names: an entity's `@wall_Roughness.png` is still that, and the runtime reads its
 * cooked file in its place while one is there that was cooked of the file as it is now, with the settings as they are
 * now (Graph::textureImports). A pack (Graph::texturePacks) is a picture of its own made of others' channels — the
 * metalness, roughness and occlusion of a material as one — and each picture it is made of is, to whoever holds it,
 * the pack's channel it went into, in every channel: what reads the file reads that, from the one picture.
 */

#include <kasset/texture.h>

namespace kgraph
{
    using kasset::TextureFormat;
    using kasset::TextureQuality;
    using kasset::TextureImport;
    using kasset::TexturePack;
    using kasset::TextureKind;
    using kasset::DefaultTextureFormat;
    using kasset::IsTexturePack;
    using kasset::SuggestTexturePacks;
    using kasset::TextureCompressible;
    using kasset::TextureFormatName;
    using kasset::TextureFormatNamed;
    using kasset::TextureFormats;
    using kasset::TextureKindName;
    using kasset::TextureKindNamed;
    using kasset::TextureKindOf;
    using kasset::TextureQualityName;
    using kasset::TextureQualityNamed;
}
