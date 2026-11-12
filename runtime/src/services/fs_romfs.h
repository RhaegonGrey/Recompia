// SPDX-License-Identifier: GPL-3.0-or-later
// A RomFS image synthesised from a host folder: the byte-addressed storage nn::fs::MountRom reads (fsp-srv OpenDataStorageByCurrentProcess
// returns it as an IStorage; the SDK parses the RomFS tables itself). Nothing is copied: the metadata tables are built in memory and
// file data is read from the host files on demand.
//
// Image layout (https://switchbrew.org/wiki/RomFS, libnx romfs_dev / yuzu fsmitm_romfsbuild): the 0x50-byte header at offset 0 (padded
// to 0x200), the file data partition from 0x200 (each file starts at a multiple of 0x10 from the partition start, in the order of the
// file table), then the directory hash table, directory table, file hash table and file table, each 4-byte aligned; the header holds
// the offsets and sizes. Entries are sorted by name (byte order) within a directory.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace nxrt::services::fs {

class RomfsImage {
public:
    RomfsImage();
    ~RomfsImage();
    // Scans `host_dir` (recursively; names are taken as UTF-8 from the host paths) and builds the tables. `host_dir` may also name a .nup
    // package (nxrt/nup.h): its RomFS is served from the package file. False with *err set when the
    // folder does not exist or an entry cannot be described. Replaces an earlier build.
    // `base_dir` (optional, a folder or a .nup): a lower layer. Every file and folder of it that `host_dir` lacks is served too, as a
    // console patch over its base game: the files of `host_dir` win. For an update dumped without the base game's files under it (an
    // update only holds the files it changes; fork #128).
    // `mod_dirs` (optional, romfs-shaped folders): the top layers, index 0 highest. Their files win over both `host_dir` and `base_dir`, and
    // over the files of a mod folder after them; shared folders merge, and files found only in a mod are served too.
    bool build(const std::string& host_dir, std::string* err = nullptr, const std::string& base_dir = std::string(),
               const std::vector<std::string>& mod_dirs = {});
    bool built() const;
    uint64_t size() const;                                  // total image size in bytes
    // Copies [offset, offset + len) of the image into dst. False when the range is outside the image or a host file cannot be read
    // (a file that shrank since build() reads as an error, not as zeros). Thread safe; any number of readers may call it at once.
    bool read(uint64_t offset, void* dst, uint64_t len) const;
    struct ModStats {
        uint32_t files = 0;                                 // files served from this mod folder
        uint32_t replaced = 0;                              // of those, the files that replace a file of the update or base game
    };
    struct Stats {
        uint32_t files = 0, dirs = 0;                       // dirs counts the root
        uint64_t data_bytes = 0;                            // sum of file sizes
        uint64_t data_offset = 0;                           // header.file_partition_ofs
        uint32_t base_files = 0;                            // files served from the base layer (build's base_dir)
        std::vector<ModStats> mods;                         // one per mod folder (build's mod_dirs), in that order
    };
    Stats stats() const;
    // Image offset and size of a file by its path inside the image ("/a/b.bin", case sensitive), for tests and logs. False when absent.
    bool file_extent(const std::string& path, uint64_t* offset, uint64_t* size) const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace nxrt::services::fs
