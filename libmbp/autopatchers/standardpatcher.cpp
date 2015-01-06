/*
 * Copyright (C) 2014  Xiao-Long Chen <chenxiaolong@cxl.epac.to>
 *
 * This file is part of MultiBootPatcher
 *
 * MultiBootPatcher is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * MultiBootPatcher is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with MultiBootPatcher.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "autopatchers/standardpatcher.h"

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/format.hpp>

#include "private/fileutils.h"
#include "private/regex.h"


/*! \cond INTERNAL */
class StandardPatcher::Impl
{
public:
    const PatcherConfig *pc;
    const FileInfo *info;
};
/*! \endcond */


const std::string StandardPatcher::Id
        = "StandardPatcher";

const std::string StandardPatcher::UpdaterScript
        = "META-INF/com/google/android/updater-script";
static const std::string Mount
        = "run_program(\"/update-binary-tool\", \"mount\", \"%1%\"};";
static const std::string Unmount
        = "run_program(\"/update-binary-tool\", \"unmount\", \"%1%\"};";
static const std::string Format
        = "run_program(\"/update-binary-tool\", \"format\", \"%1%\"};";

static const std::string System = "system";
static const std::string Cache = "cache";
static const std::string Data = "data";


StandardPatcher::StandardPatcher(const PatcherConfig * const pc,
                                 const FileInfo * const info,
                                 const PatchInfo::AutoPatcherArgs &args) :
    m_impl(new Impl())
{
    (void) args;

    m_impl->pc = pc;
    m_impl->info = info;
}

StandardPatcher::~StandardPatcher()
{
}

PatcherError StandardPatcher::error() const
{
    return PatcherError();
}

std::string StandardPatcher::id() const
{
    return Id;
}

std::vector<std::string> StandardPatcher::newFiles() const
{
    return std::vector<std::string>();
}

std::vector<std::string> StandardPatcher::existingFiles() const
{
    std::vector<std::string> files;
    files.push_back(UpdaterScript);
    return files;
}

bool StandardPatcher::patchFiles(const std::string &directory,
                                 const std::vector<std::string> &bootImages)
{
    (void) bootImages;

    std::vector<unsigned char> contents;

    // UpdaterScript begin
    FileUtils::readToMemory(directory + "/" + UpdaterScript, &contents);
    std::string strContents(contents.begin(), contents.end());
    std::vector<std::string> lines;
    boost::split(lines, strContents, boost::is_any_of("\n"));

    replaceMountLines(&lines, m_impl->info->device());
    replaceUnmountLines(&lines, m_impl->info->device());
    replaceFormatLines(&lines, m_impl->info->device());

    // Remove device check if requested
    std::string key = m_impl->info->patchInfo()->keyFromFilename(
            m_impl->info->filename());
    if (!m_impl->info->patchInfo()->deviceCheck(key)) {
        removeDeviceChecks(&lines);
    }

    strContents = boost::join(lines, "\n");
    contents.assign(strContents.begin(), strContents.end());
    FileUtils::writeFromMemory(directory + "/" + UpdaterScript, contents);
    // UpdaterScript end

    return true;
}

/*!
    \brief Disable assertions for device model/name in updater-script

    \param lines Container holding strings of lines in updater-script file
 */
void StandardPatcher::removeDeviceChecks(std::vector<std::string> *lines)
{
    MBP_regex reLine("^\\s*assert\\s*\\(.*getprop\\s*\\(.*(ro.product.device|ro.build.product)");

    for (auto &line : *lines) {
        if (MBP_regex_search(line, reLine)) {
            MBP_regex_replace(line, MBP_regex("^(\\s*assert\\s*\\()"),
                              "\\1\"true\" == \"true\" || ");
        }
    }
}

/*!
    \brief Change partition mounting lines to be multiboot-compatible

    \param lines Container holding strings of lines in updater-script file
    \param device Target device (needed for /dev names)
 */
void StandardPatcher::replaceMountLines(std::vector<std::string> *lines,
                                        Device *device)
{
    auto const pSystem = device->partition(System);
    auto const pCache = device->partition(Cache);
    auto const pData = device->partition(Data);

    static auto const re1 = MBP_regex("^\\s*mount\\s*\\(.*$");
    static auto const re2 =
            MBP_regex("^\\s*run_program\\s*\\(\\s*\"[^\"]*busybox\"\\s*,\\s*\"mount\".*$");
    static auto const re3 =
            MBP_regex("^\\s*run_program\\s*\\(\\s*\"[^\",]*/mount\".*$");

    for (auto it = lines->begin(); it != lines->end(); ++it) {
        auto const &line = *it;

        bool isMountLine = MBP_regex_search(line, re1)
                || MBP_regex_search(line, re2)
                || MBP_regex_search(line, re3);

        if (isMountLine) {
            bool isSystem = line.find(System) != std::string::npos
                    || (!pSystem.empty() && line.find(pSystem) != std::string::npos);
            bool isCache = line.find(Cache) != std::string::npos
                    || (!pCache.empty() && line.find(pCache) != std::string::npos);
            bool isData = line.find(Data) != std::string::npos
                    || line.find("userdata") != std::string::npos
                    || (!pData.empty() && line.find(pData) != std::string::npos);

            if (isSystem) {
                it = lines->erase(it);
                it = lines->insert(it, (boost::format(Mount) % "/system").str());
            } else if (isCache) {
                it = lines->erase(it);
                it = lines->insert(it, (boost::format(Mount) % "/cache").str());
            } else if (isData) {
                it = lines->erase(it);
                it = lines->insert(it, (boost::format(Mount) % "/data").str());
            }
        }
    }
}

/*!
    \brief Change partition unmounting lines to be multiboot-compatible

    \param lines Container holding strings of lines in updater-script file
    \param device Target device (needed for /dev names)
 */
void StandardPatcher::replaceUnmountLines(std::vector<std::string> *lines,
                                          Device *device)
{
    auto const pSystem = device->partition(System);
    auto const pCache = device->partition(Cache);
    auto const pData = device->partition(Data);

    static auto const re1 = MBP_regex("^\\s*unmount\\s*\\(.*$");
    static auto const re2 =
            MBP_regex("^\\s*run_program\\s*\\(\\s*\"[^\"]*busybox\"\\s*,\\s*\"umount\".*$");

    for (auto it = lines->begin(); it != lines->end(); ++it) {
        auto const &line = *it;

        bool isUnmountLine = MBP_regex_search(line, re1)
                || MBP_regex_search(line, re2);

        if (isUnmountLine) {
            bool isSystem = line.find(System) != std::string::npos
                    || (!pSystem.empty() && line.find(pSystem) != std::string::npos);
            bool isCache = line.find(Cache) != std::string::npos
                    || (!pCache.empty() && line.find(pCache) != std::string::npos);
            bool isData = line.find(Data) != std::string::npos
                    || line.find("userdata") != std::string::npos
                    || (!pData.empty() && line.find(pData) != std::string::npos);

            if (isSystem) {
                it = lines->erase(it);
                it = lines->insert(it, (boost::format(Unmount) % "/system").str());
            } else if (isCache) {
                it = lines->erase(it);
                it = lines->insert(it, (boost::format(Unmount) % "/cache").str());
            } else if (isData) {
                it = lines->erase(it);
                it = lines->insert(it, (boost::format(Unmount) % "/data").str());
            }
        }
    }
}

/*!
    \brief Change partition formatting lines to be multiboot-compatible

    \param lines Container holding strings of lines in updater-script file
    \param device Target device (needed for /dev names)
 */
void StandardPatcher::replaceFormatLines(std::vector<std::string> *lines,
                                         Device *device)
{
    auto const pSystem = device->partition(System);
    auto const pCache = device->partition(Cache);
    auto const pData = device->partition(Data);

    for (auto it = lines->begin(); it != lines->end(); ++it) {
        auto const &line = *it;

        if (MBP_regex_search(line, MBP_regex("^\\s*format\\s*\\(.*$"))) {
            bool isSystem = line.find(System) != std::string::npos
                    || (!pSystem.empty() && line.find(pSystem) != std::string::npos);
            bool isCache = line.find(Cache) != std::string::npos
                    || (!pCache.empty() && line.find(pCache) != std::string::npos);
            bool isData = line.find(Data) != std::string::npos
                    || line.find("userdata") != std::string::npos
                    || (!pData.empty() && line.find(pData) != std::string::npos);

            if (isSystem) {
                it = lines->erase(it);
                it = lines->insert(it, (boost::format(Format) % "/system").str());
            } else if (isCache) {
                it = lines->erase(it);
                it = lines->insert(it, (boost::format(Format) % "/cache").str());
            } else if (isData) {
                it = lines->erase(it);
                it = lines->insert(it, (boost::format(Format) % "/data").str());
            }
        } else if (MBP_regex_search(line, MBP_regex(
                "delete_recursive\\s*\\([^\\)]*\"/system\""))) {
            it = lines->erase(it);
            it = lines->insert(it, (boost::format(Format) % "/system").str());
        } else if (MBP_regex_search(line, MBP_regex(
                "delete_recursive\\s*\\([^\\)]*\"/cache\""))) {
            it = lines->erase(it);
            it = lines->insert(it, (boost::format(Format) % "/cache").str());
        } else if (MBP_regex_search(line, MBP_regex(
                "^\\s*run_program\\s*\\(\\s*\"[^\",]*/format.sh\".*$"))) {
            it = lines->erase(it);
            it = lines->insert(it, (boost::format(Format) % "/data").str());
        }
    }
}
