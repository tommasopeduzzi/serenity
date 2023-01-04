/*
 * Copyright (c) 2022, kleines Filmröllchen <filmroellchen@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Slide.h"
#include <AK/DeprecatedString.h>
#include <AK/Forward.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Vector.h>
#include <LibGfx/Painter.h>
#include <LibGfx/Size.h>

static constexpr int const PRESENTATION_FORMAT_VERSION = 1;

struct Metadata {
    DeprecatedString author;
    DeprecatedString title;
    DeprecatedString last_modified;
    int width;
    DeprecatedString aspect_ratio;
};

// In-memory representation of the presentation stored in a file.
// This class also contains all the parser code for loading .presenter files.
class Presentation {
public:
    ~Presentation() = default;

    // We can't pass this class directly in an ErrorOr because some of the components are not properly moveable under these conditions.
    static ErrorOr<NonnullOwnPtr<Presentation>> load_from_file(StringView file_name, NonnullRefPtr<GUI::Window> window);

    StringView file_path() const { return m_file_path; }
    StringView title() const;
    StringView author() const;
    Gfx::IntSize normative_size() const { return m_normative_size; }

    Slide& current_slide() { return m_slides[m_current_slide.value()]; }
    unsigned current_slide_number() const { return m_current_slide.value(); }
    unsigned current_frame_in_slide_number() const { return m_current_frame_in_slide.value(); }

    void set_file_path(DeprecatedString file_path) { m_file_path = move(file_path); }

    void next_frame();
    void previous_frame();
    void go_to_first_slide();

    // This assumes that the caller has clipped the painter to exactly the display area.
    void paint(Gfx::Painter& painter);

private:
    static Metadata parse_metadata(JsonObject const& metadata_object);
    static ErrorOr<Gfx::IntSize> parse_presentation_size(JsonObject const& metadata_object);

    Presentation(Gfx::IntSize normative_size, Metadata metadata, DeprecatedString file_path);
    static NonnullOwnPtr<Presentation> construct(Gfx::IntSize normative_size, Metadata metadata, DeprecatedString file_path);

    void append_slide(Slide slide);

    DeprecatedString m_file_path;

    Vector<Slide> m_slides {};
    // This is not a pixel size, but an abstract size used by the slide objects for relative positioning.
    Gfx::IntSize m_normative_size;
    Metadata m_metadata;

    Checked<unsigned> m_current_slide { 0 };
    Checked<unsigned> m_current_frame_in_slide { 0 };
};
