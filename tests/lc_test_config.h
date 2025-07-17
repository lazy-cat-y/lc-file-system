#ifndef LC_TEST_CONFIG_H
#define LC_TEST_CONFIG_H

#include <filesystem>
#include <string>

#define IMG_DIR_PATH "./test_imgs"

inline std::string get_test_img(std::string img_name) {
    return std::string(IMG_DIR_PATH) + "/" + img_name;
}

inline void init_test_img_dir() {
    if (std::filesystem::exists(IMG_DIR_PATH)) {
        std::filesystem::remove_all(IMG_DIR_PATH);
    }
    std::filesystem::create_directory(IMG_DIR_PATH);
}

inline void clear_test_img_dir() {
    if (std::filesystem::exists(IMG_DIR_PATH)) {
        std::filesystem::remove_all(IMG_DIR_PATH);
    }
    std::filesystem::remove(IMG_DIR_PATH);
}

#endif
