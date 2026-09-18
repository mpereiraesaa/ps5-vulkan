/*
 * Copyright (C) 2026 Manuel Pereira
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Reference-image loader for the packaged upstream CTS.
 *
 * The pinned modules that compare against a reference image call
 * tcu::ImageIO::loadPNG with the image's name in the data archive. Upstream
 * implements that with libpng; this integration carries no PNG decoder, so the
 * build decodes the pinned assets once (tools/embed_cts_reference_images.py) and
 * this translation unit serves the same bytes back through the same entry point.
 * The comparison therefore runs against the verbatim upstream pixels: the image
 * format changed, the oracle did not. An unknown name is a hard error rather
 * than an empty image, so a module that asks for something the payload does not
 * carry fails loudly instead of comparing against nothing.
 */
#include "tcuImageIO.hpp"
#include "tcuTexture.hpp"
#include "tcuDefs.hpp"
#include <cstring>
#include <string>

namespace ps5cts {
struct ReferenceImage {
    const char *name;
    unsigned width;
    unsigned height;
    const unsigned char *pixels;
};
extern const ReferenceImage kGeometryReferenceImages[];
extern const unsigned kGeometryReferenceImageCount;
} // namespace ps5cts

namespace tcu {
namespace ImageIO {

void loadPNG(TextureLevel &dst, const Archive &archive, const char *fileName)
{
    (void)archive;
    const char *base = fileName;
    for (const char *cursor = fileName; cursor && *cursor; ++cursor)
        if (*cursor == '/')
            base = cursor + 1;
    std::string name(base ? base : "");
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".png") == 0)
        name.resize(name.size() - 4);
    for (unsigned index = 0; index < ps5cts::kGeometryReferenceImageCount; ++index) {
        const ps5cts::ReferenceImage &image = ps5cts::kGeometryReferenceImages[index];
        if (name != image.name)
            continue;
        dst.setStorage(TextureFormat(TextureFormat::RGBA, TextureFormat::UNORM_INT8),
                       (int)image.width, (int)image.height);
        const ConstPixelBufferAccess access = dst.getAccess();
        for (unsigned y = 0; y < image.height; ++y)
            std::memcpy((unsigned char *)access.getDataPtr() + (size_t)y * access.getRowPitch(),
                        image.pixels + (size_t)y * image.width * 4u, (size_t)image.width * 4u);
        return;
    }
    throw InternalError("no packaged reference image for " + name);
}

} // namespace ImageIO
} // namespace tcu
