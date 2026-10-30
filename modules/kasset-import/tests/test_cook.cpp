// A picture cooked: kept by its content and settings, so that a checkout of the same picture is not cooked again.

#include <gtest/gtest.h>

#include <kasset/cook.h>
#include <kasset/id.h>

#include <chrono>
#include <format>
#include <fstream>

using namespace kasset;
namespace fs = std::filesystem;

TEST(Cook, KeptByContentAndSettings)
{
    const fs::path root = fs::temp_directory_path() / std::format("kasset-cook-{}", Id::New().Text());
    fs::create_directories(root);
    std::ofstream(root / "wall.png") << "pixels";
    const cook::FileOf fileOf = [&](const std::string_view id) { return root / std::string(id.substr(1)); };
    const cook::Recipe recipe { "@wall.png", TextureFormat::eBC7 };

    const auto first = cook::CookedPath(root / "cache", recipe, fileOf);
    ASSERT_TRUE(first);
    EXPECT_TRUE(first->filename().string().starts_with("wall-"));
    fs::last_write_time(root / "wall.png", fs::last_write_time(root / "wall.png") + std::chrono::hours(2));
    EXPECT_EQ(cook::CookedPath(root / "cache", recipe, fileOf), first) << "touched, as a checkout does, it is the same";

    cook::Recipe other = recipe;
    other.format = TextureFormat::eBC1;
    EXPECT_NE(cook::CookedPath(root / "cache", other, fileOf), first);
    std::ofstream(root / "wall.png") << "other pixels";
    EXPECT_NE(cook::CookedPath(root / "cache", recipe, fileOf), first);
    EXPECT_FALSE(cook::CookedPath(root / "cache", { "@gone.png", TextureFormat::eBC7 }, fileOf));
    fs::remove_all(root);
}
