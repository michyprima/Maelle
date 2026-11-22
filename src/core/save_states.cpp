/*
    Copyright 2019-2026 Hydr8gon
    Copyright 2026 michyprima

    This file is part of Maelle.

    Maelle is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Maelle is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Maelle. If not, see <https://www.gnu.org/licenses/>.
*/

#include "core.h"
#include "file_lock.h"

const char *SaveStates::stateTag = "NOOD";
const uint32_t SaveStates::stateVersion = 24;

// States from older versions back to this one still load; what they lack gets the values it has after power-up
// To add to a component's state, bump stateVersion, write the new values in saveState, and in loadState read them only
// if the version is at least the new one, setting them as after power-up otherwise
const uint32_t SaveStates::oldestVersion = 23;

void SaveStates::setPath(std::string path, bool gba) {
    // Set the NDS or GBA state path
    (gba ? gbaPath : ndsPath) = path;
}

void SaveStates::setFd(int fd, bool gba) {
    // Set the NDS or GBA state descriptor
    (gba ? gbaFd : ndsFd) = fd;
}

FILE *SaveStates::openFile(const char *mode) {
    // Open the NDS or GBA state file based on what's running
    if (gbaFd != -1 && (core->gbaMode || ndsFd == -1))
        return fdopen(dup(gbaFd), mode);
    else if (ndsFd != -1)
        return fdopen(dup(ndsFd), mode);
    else if (gbaPath != "" && (core->gbaMode || ndsPath == ""))
        return fopen(gbaPath.c_str(), mode);
    else if (ndsPath != "")
        return fopen(ndsPath.c_str(), mode);
    return nullptr;
}

StateResult SaveStates::checkState() {
    std::lock_guard<FileLock> guard(fileLock());
    // Try to open the state file, if it exists
    FILE *file = openFile("rb");
    if (!file) return STATE_FILE_FAIL;
    fseek(file, 0, SEEK_END);
    uint32_t size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size == 0) return STATE_FILE_FAIL;

    // Get header values from the file for comparison
    uint8_t tag[4];
    uint32_t version;
    fread(tag, sizeof(uint8_t), 4, file);
    fread(&version, sizeof(uint32_t), 1, file);
    fclose(file);

    // Check if the format tag matches
    for (int i = 0; i < 4; i++)
        if (tag[i] != stateTag[i])
            return STATE_FORMAT_FAIL;

    // Check if the state version is one that can be loaded
    if (version < oldestVersion || version > stateVersion)
        return STATE_VERSION_FAIL;
    return STATE_SUCCESS;
}

bool SaveStates::saveState() {
    std::lock_guard<FileLock> guard(fileLock());
    // Open the state file and write the header
    FILE *file = openFile("wb");
    if (!file) return false;
    fwrite(stateTag, sizeof(uint8_t), 4, file);
    fwrite(&stateVersion, sizeof(uint32_t), 1, file);

    // Save the state of every component, with a noted word from the DS cart as a scheduled task like it used to be
    core->cartridgeNds.flushWord();
    core->saveState(file);
    core->aes.saveState(file);
    core->cartridgeGba.saveState(file);
    core->cartridgeNds.saveState(file);
    core->cp15.saveState(file);
    core->divSqrt.saveState(file);
    core->dma[0].saveState(file);
    core->dma[1].saveState(file);
    core->gpu.saveState(file);
    core->gpu2D[0].saveState(file);
    core->gpu2D[1].saveState(file);
    core->gpu3D.saveState(file);
    core->gpu3DRenderer.saveState(file);
    core->hleBios[0].saveState(file);
    core->hleBios[1].saveState(file);
    core->hleBios[2].saveState(file);
    core->i2c.saveState(file);
    core->interpreter[0].saveState(file);
    core->interpreter[1].saveState(file);
    core->input.saveState(file);
    core->ipc.saveState(file);
    core->memory.saveState(file);
    core->ndma[0].saveState(file);
    core->ndma[1].saveState(file);
    core->rtc.saveState(file);
    core->sdMmc.saveState(file);
    core->sdio.saveState(file);
    core->dsiWifi.saveState(file);
    core->dsiMic.saveState(file);
    core->dsiGpio.saveState(file);
    core->dsiCamera.saveState(file);
    core->dsiDsp.saveState(file);
    core->spi.saveState(file);
    core->spu.saveState(file);
    core->timers[0].saveState(file);
    core->timers[1].saveState(file);
    core->wifi.saveState(file);
    core->slot2.saveState(file);
    fclose(file);
    return true;
}

bool SaveStates::loadState() {
    std::lock_guard<FileLock> guard(fileLock());
    // Open the state file and read past the header, keeping the version that components read their state for
    FILE *file = openFile("rb");
    if (!file) return false;
    uint32_t version;
    fseek(file, 4, SEEK_SET);
    fread(&version, sizeof(uint32_t), 1, file);

    // Drop compiled code, since memory is about to change underneath it
    core->jit.reset();
    core->jit7.reset();

    // Load the state of every component
    core->loadState(file, version);
    core->aes.loadState(file, version);
    core->cartridgeGba.loadState(file, version);
    core->cartridgeNds.loadState(file, version);
    core->cp15.loadState(file, version);
    core->divSqrt.loadState(file, version);
    core->dma[0].loadState(file, version);
    core->dma[1].loadState(file, version);
    core->gpu.loadState(file, version);
    core->gpu2D[0].loadState(file, version);
    core->gpu2D[1].loadState(file, version);
    core->gpu3D.loadState(file, version);
    core->gpu3DRenderer.loadState(file, version);
    core->hleBios[0].loadState(file, version);
    core->hleBios[1].loadState(file, version);
    core->hleBios[2].loadState(file, version);
    core->i2c.loadState(file, version);
    core->interpreter[0].loadState(file, version);
    core->interpreter[1].loadState(file, version);
    core->input.loadState(file, version);
    core->ipc.loadState(file, version);
    core->memory.loadState(file, version);
    core->ndma[0].loadState(file, version);
    core->ndma[1].loadState(file, version);
    core->rtc.loadState(file, version);
    core->sdMmc.loadState(file, version);
    core->sdio.loadState(file, version);
    core->dsiWifi.loadState(file, version);
    core->dsiMic.loadState(file, version);
    core->dsiGpio.loadState(file, version);
    core->dsiCamera.loadState(file, version);
    core->dsiDsp.loadState(file, version);
    core->spi.loadState(file, version);
    core->spu.loadState(file, version);
    core->timers[0].loadState(file, version);
    core->timers[1].loadState(file, version);
    core->wifi.loadState(file, version);
    core->slot2.loadState(file, version);
    fclose(file);

    // Pick which CPUs run again now that their halt states are loaded; the core picked it before loading them, from
    // the halt states before the load, which could leave CPUs that should run stopped until something halted one
    core->updateRun();
    return true;
}
