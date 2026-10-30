#pragma once

/**
 * @file beside.h
 * @brief What a model names beside it: the files that go with it wherever it goes — copied into a project, moved.
 */

#include <filesystem>
#include <vector>

namespace kasset
{
    /**
     * @brief The files a model names beside it, by their paths from its folder: a glTF's buffers and pictures, an OBJ's
     *        material files and the pictures they name. Only those beside it or under it: not one by a whole path, a
     *        `..`, or a URL, nor one in the file itself (`data:`).
     */
    [[nodiscard]] std::vector<std::filesystem::path> NamedBeside(const std::filesystem::path& model);
}
