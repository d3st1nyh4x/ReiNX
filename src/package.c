/*
* Copyright (c) 2018 Reisyukaku
*
* This program is free software; you can redistribute it and/or modify it
* under the terms and conditions of the GNU General Public License,
* version 2, as published by the Free Software Foundation.
*
* This program is distributed in the hope it will be useful, but WITHOUT
* ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
* FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
* more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "hwinit.h"
#include "error.h"
#include "fs.h"
#include "package.h"
#include "patches.h"
#include "kippatches/fs.inc"

bool customSecmon = false;
bool customWb = false;
bool customKernel = false;

u8 *ReadBoot0(sdmmc_storage_t *storage){
    u8 *bctBuf = (u8 *)malloc(0x4000);
    sdmmc_storage_read(storage, 0 , 0x4000 / NX_EMMC_BLOCKSIZE, bctBuf);
    return bctBuf;
}

u8 *ReadPackage1Ldr(sdmmc_storage_t *storage) {
    u8 *pk11 = malloc(0x40000);
    sdmmc_storage_read(storage, 0x100000 / NX_EMMC_BLOCKSIZE, 0x40000 / NX_EMMC_BLOCKSIZE, pk11);
    return pk11;
}

u8 *ReadPackage2(sdmmc_storage_t *storage, size_t *out_size) {
    // Read GPT partition.
    LIST_INIT(gpt);
    sdmmc_storage_set_mmc_partition(storage, 0);
    print("Parsing GPT...\n");
    nx_emmc_gpt_parse(&gpt, storage);
    emmc_part_t *pkg2_part = nx_emmc_part_find(&gpt, "BCPKG2-1-Normal-Main");
    nx_emmc_gpt_free(&gpt);
    if (!pkg2_part) {
        error("Failed to read GPT!\n");
        return 0;
    }

    // Read Package2.
    u8 *tmp = (u8 *)malloc(NX_EMMC_BLOCKSIZE);
    print("Reading Package2 size...\n");
    nx_emmc_part_read(storage, pkg2_part, 0x4000 / NX_EMMC_BLOCKSIZE, 1, tmp);
    u32 *hdr = (u32 *)(tmp + 0x100);
    u32 pkg2_size = hdr[0] ^ hdr[2] ^ hdr[3];
    *out_size = pkg2_size;
    free(tmp);
    u8 *pkg2 = malloc(ALIGN(pkg2_size, NX_EMMC_BLOCKSIZE));
    print("Reading Package2...\n");
    u32 ret = nx_emmc_part_read(storage, pkg2_part, 0x4000 / NX_EMMC_BLOCKSIZE, ALIGN(pkg2_size, NX_EMMC_BLOCKSIZE) / NX_EMMC_BLOCKSIZE, pkg2);
    sdmmc_storage_end(storage);
    if (!ret) {
        error("Failed to read Package2!\n");
        return 0;
    }
    return pkg2;
}

pkg2_hdr_t *unpackFirmwarePackage(u8 *data) {
    print("Unpacking firmware...\n");
    pkg2_hdr_t *hdr = (pkg2_hdr_t *)(data + 0x100);

    //Decrypt header.
    se_aes_crypt_ctr(8, hdr, sizeof(pkg2_hdr_t), hdr, sizeof(pkg2_hdr_t), hdr);

    if (hdr->magic != PKG2_MAGIC) {
        error("Package2 Magic invalid!\nThere is a good chance your ReiNX build is outdated\nPlease get the newest build from our guide (reinx.guide) or our discord (discord.reiswitched.team)\nMake sure you replace the ReiNX.bin file on your SD card root too\n");
        return NULL;
    }

    //Decrypt body
    data += (0x100 + sizeof(pkg2_hdr_t));

    for (u32 i = 0; i < 4; i++) {
        if (!hdr->sec_size[i]) continue;

        se_aes_crypt_ctr(8, data, hdr->sec_size[i], data, hdr->sec_size[i], &hdr->sec_ctr[i * 0x10]);

        data += hdr->sec_size[i];
    }

    return hdr;
}

u8 *LoadExtFile(char *path, size_t *size) {
    print("Reading external file %s\n", path);
    u8 *buf = NULL;
    if(fopen(path, "rb") != 0) {
        size_t fileSize = fsize();
        *size = fileSize;
        if(fileSize <= 0) {
            error("File is empty!\n");
            fclose();
            return NULL;
        }
        buf = malloc(fileSize);
        fread(buf, fileSize, 1);
        fclose();
    }
    return buf;
}

void pkg1_unpack(pk11_offs *offs, u32 pkg1Off) {
    u8 ret = 0;

    pk11_header *hdr = (pk11_header *)(pkg1Off + 0x20);

    u32 sec_size[3] = { hdr->wb_size, hdr->ldr_size, hdr->sm_size };
    u8 *pdata = (u8 *)hdr + sizeof(pk11_header);

    for (u32 i = 0; i < 3; i++) {
        if (offs->sec_map[i] == 0 && offs->warmboot_base) {
            size_t extSize = 0;
            u8 *extWb = LoadExtFile("/ReiNX/warmboot.bin", &extSize);
            if(extWb == NULL) extWb = LoadExtFile("/ReiNX/lp0fw.bin", &extSize);
            if(offs->kb >= KB_FIRMWARE_VERSION_700 && extWb == NULL)
                error("Custom warmboot required!");
            memcpy((void *)offs->warmboot_base, extWb == NULL ? pdata : extWb, extWb == NULL ? sec_size[offs->sec_map[i]] : extSize);
            free(extWb);
        }
        if (offs->sec_map[i] == 2 && offs->secmon_base) {
            size_t extSize = 0;
            u8 *extSec = LoadExtFile("/ReiNX/secmon.bin", &extSize);
            if(extSec == NULL) extSec = LoadExtFile("/ReiNX/exosphere.bin", &extSize);
            if(offs->kb >= KB_FIRMWARE_VERSION_700 && extSec == NULL)
                error("Custom secmon required!");
            memcpy((u8 *)offs->secmon_base, extSec == NULL ? pdata : extSec, extSec == NULL ? sec_size[offs->sec_map[i]] : extSize);
            free(extSec);
        }
        pdata += sec_size[offs->sec_map[i]];
    }
}

bool hasCustomWb() {
        if (customWb)
            return customWb;
    if(fopen("/ReiNX/warmboot.bin", "rb") != 0) {
        customWb = true;
        fclose();
    }
    if(fopen("/ReiNX/lp0fw.bin", "rb") != 0) {
        customWb = true;
        fclose();
    }
    return customWb;
}

bool hasCustomSecmon() {
        if (customSecmon)
            return customSecmon;
    if(fopen("/ReiNX/secmon.bin", "rb") != 0) {
        customSecmon = true;
        fclose();
    }
    if(fopen("/ReiNX/exosphere.bin", "rb") != 0) {
        customSecmon = true;
        fclose();
    }
    return customSecmon;
}

bool hasCustomKern() {
    if (customKernel)
            return customKernel;
    if(fopen("/ReiNX/kernel.bin", "rb") != 0) {
        customKernel = true;
        fclose();
    }
    return customKernel;
}


static u32 buildIni1(pkg2_hdr_t *hdr, void *ini, link_t *kips_info, bool hasCustSecmon)
{
    u32 ini1_size = sizeof(pkg2_ini1_t);
    pkg2_ini1_t *ini1 = (pkg2_ini1_t *)ini;
    memset(ini1, 0, sizeof(pkg2_ini1_t));
    ini1->magic = INI1_MAGIC;
    ini += sizeof(pkg2_ini1_t);
    LIST_FOREACH_ENTRY(pkg2_kip1_info_t, ki, kips_info, link)
    {
        print("adding kip1 '%s' @ %08X (%08X)\n", ki->kip1->name, (u32)ki->kip1, ki->size);
        memcpy(ini, ki->kip1, ki->size);
        ini += ki->size;
        ini1_size += ki->size;
        ini1->num_procs++;
    }
    ini1->size = ini1_size;
    
    if (!hasCustSecmon) 
        se_aes_crypt_ctr(8, ini1, ini1_size, ini1, ini1_size, &hdr->sec_ctr[PKG2_SEC_INI1 * 0x10]);
    
    return ini1_size;
}

void buildFirmwarePackage(u8 *kernel, u32 kernel_size, link_t *kips_info, pk11_offs *pk11Offs) {
    u8 *pdst = (u8 *)0xA9800000;
    bool hasCustSecmon = hasCustomSecmon();
    bool new_pkg2 = pk11Offs->hos >= HOS_FIRMWARE_VERSION_800;

    //Signature.
    memset(pdst, 0, 0x100);
    pdst += 0x100;

    //Header.
    pkg2_hdr_t *hdr = (pkg2_hdr_t *)pdst;
    memset(hdr, 0, sizeof(pkg2_hdr_t));
    pdst += sizeof(pkg2_hdr_t);
    hdr->magic = PKG2_MAGIC;
    hdr->base = new_pkg2 ? 0x60000 : 0x10000000;

    //Read custom kern if applicable
    size_t extSize = 0;
    u8 *extKern = LoadExtFile("/ReiNX/kernel.bin", &extSize);
    memcpy(hdr->data, extKern == NULL ? kernel : extKern, extKern == NULL ? kernel_size : extSize);
    print("Kernel size: %X\n", kernel_size);
    
    //Encrpyt kern if no exo
    if(!hasCustSecmon)
        se_aes_crypt_ctr(8, hdr->data, kernel_size, hdr->data, kernel_size, &hdr->sec_ctr[PKG2_SEC_KERNEL * 0x10]);
    pdst += kernel_size;
    
    //Build ini1
    size_t iniSize = buildIni1(hdr, pdst, kips_info, hasCustSecmon); 
    
    //Newer (8.0+) pk21 embeds ini1 in kernel section, so add ini1 size to kernel size
    if (new_pkg2) {
        *(u32*)(hdr->data + kernelInfo[8].krnl_offs) = kernel_size; //TODO
        kernel_size += iniSize;
    }
    
    //Fill in rest of the header
    hdr->sec_off[PKG2_SEC_KERNEL] = hdr->base;
    hdr->sec_size[PKG2_SEC_KERNEL] = kernel_size;
    hdr->sec_off[PKG2_SEC_INI1] = new_pkg2 ? 0 : 0x14080000;
    hdr->sec_size[PKG2_SEC_INI1] = new_pkg2 ? 0 : iniSize;

    // Encrypt header.
    *(u32 *)hdr->ctr = 0x100 + sizeof(pkg2_hdr_t) + kernel_size + hdr->sec_size[PKG2_SEC_INI1];
    if (!hasCustSecmon)
        se_aes_crypt_ctr(8, hdr, sizeof(pkg2_hdr_t), hdr, sizeof(pkg2_hdr_t), hdr);
    memset(hdr->ctr, 0 , 0x10);
    *(u32 *)hdr->ctr = 0x100 + sizeof(pkg2_hdr_t) + kernel_size + hdr->sec_size[PKG2_SEC_INI1];
}

size_t calcKipSize(pkg2_kip1_t *kip1) {
    u32 size = sizeof(pkg2_kip1_t);
    for (u32 j = 0; j < KIP1_NUM_SECTIONS; j++)
        size += kip1->sections[j].size_comp;
    return size;
}

void pkg2_parse_kips(link_t *info, pkg2_hdr_t *pkg2) {
    u8 *ptr = pkg2->data + pkg2->sec_size[PKG2_SEC_KERNEL];
    if (pkg2->sec_size[PKG2_SEC_INI1] == 0)
        ptr = pkg2->data + *(u32 *)(pkg2->data + 0x168);
    pkg2_ini1_t *ini1 = (pkg2_ini1_t *)ptr;
    ptr += sizeof(pkg2_ini1_t);

    for (u32 i = 0; i < ini1->num_procs; i++) {
        pkg2_kip1_t *kip1 = (pkg2_kip1_t *)ptr;
        pkg2_kip1_info_t *ki = (pkg2_kip1_info_t *)malloc(sizeof(pkg2_kip1_info_t));
        ki->kip1 = kip1;
        ki->size = calcKipSize(kip1);
        list_append(info, &ki->link);
        ptr += ki->size;
    }
}

void loadKip(link_t *info, char *path) {
    if(fopen(path, "rb") == 0) return;
    pkg2_kip1_t *ckip = malloc(fsize());
    fread(ckip, fsize(), 1);
    fclose();
    LIST_FOREACH_ENTRY(pkg2_kip1_info_t, ki, info, link) {
        if (ki->kip1->tid == ckip->tid) {
            ki->kip1 = ckip;
            ki->size = calcKipSize(ckip);
            return;
        }
    }
    pkg2_kip1_info_t *ki = malloc(sizeof(pkg2_kip1_info_t));
    ki->kip1 = ckip;
    ki->size = calcKipSize(ckip);
    list_append(info, &ki->link);
}

// TODO: get full hashes somewhere and not just the first 16 bytes
// every second one is the exfat version
kippatchset_t kip_patches[] = {
    { "FS", "\xde\x9f\xdd\xa4\x08\x5d\xd5\xfe\x68\xdc\xb2\x0b\x41\x09\x5b\xb4", fs_kip_patches_100 },
    { "FS", "\xfc\x3e\x80\x99\x1d\xca\x17\x96\x4a\x12\x1f\x04\xb6\x1b\x17\x5e", fs_kip_patches_100 },
    { "FS", "\xcd\x7b\xbe\x18\xd6\x13\x0b\x28\xf6\x2f\x19\xfa\x79\x45\x53\x5b", fs_kip_patches_200 },
    { "FS", "\xe7\x66\x92\xdf\xaa\x04\x20\xe9\xfd\xd6\x8e\x43\x63\x16\x18\x18", fs_kip_patches_200 },
    { "FS", "\x0d\x70\x05\x62\x7b\x07\x76\x7c\x0b\x96\x3f\x9a\xff\xdd\xe5\x66", fs_kip_patches_210 },
    { "FS", "\xdb\xd8\x5f\xca\xcc\x19\x3d\xa8\x30\x51\xc6\x64\xe6\x45\x2d\x32", fs_kip_patches_210 },
    { "FS", "\xa8\x6d\xa5\xe8\x7e\xf1\x09\x7b\x23\xda\xb5\xb4\xdb\xba\xef\xe7", fs_kip_patches_300 },
    { "FS", "\x98\x1c\x57\xe7\xf0\x2f\x70\xf7\xbc\xde\x75\x31\x81\xd9\x01\xa6", fs_kip_patches_300 },
    { "FS", "\x57\x39\x7c\x06\x3f\x10\xb6\x31\x3f\x4d\x83\x76\x53\xcc\xc3\x71", fs_kip_patches_301 },
    { "FS", "\x07\x30\x99\xd7\xc6\xad\x7d\x89\x83\xbc\x7a\xdd\x93\x2b\xe3\xd1", fs_kip_patches_301 },
    { "FS", "\x06\xe9\x07\x19\x59\x5a\x01\x0c\x62\x46\xff\x70\x94\x6f\x10\xfb", fs_kip_patches_401 },
    { "FS", "\x54\x9b\x0f\x8d\x6f\x72\xc4\xe9\xf3\xfd\x1f\x19\xea\xce\x4a\x5a", fs_kip_patches_401 },
    { "FS", "\x80\x96\xaf\x7c\x6a\x35\xaa\x82\x71\xf3\x91\x69\x95\x41\x3b\x0b", fs_kip_patches_410 },
    { "FS", "\x02\xd5\xab\xaa\xfd\x20\xc8\xb0\x63\x3a\xa0\xdb\xae\xe0\x37\x7e", fs_kip_patches_410 },
    { "FS", "\xa6\xf2\x7a\xd9\xac\x7c\x73\xad\x41\x9b\x63\xb2\x3e\x78\x5a\x0c", fs_kip_patches_500 },
    { "FS", "\xce\x3e\xcb\xa2\xf2\xf0\x62\xf5\x75\xf8\xf3\x60\x84\x2b\x32\xb4", fs_kip_patches_500 },
    { "FS", "\x76\xf8\x74\x02\xc9\x38\x7c\x0f\x0a\x2f\xab\x1b\x45\xce\xbb\x93", fs_kip_patches_510 },
    { "FS", "\x10\xb2\xd8\x16\x05\x48\x85\x99\xdf\x22\x42\xcb\x6b\xac\x2d\xf1", fs_kip_patches_510 },
    { "FS", "\x1b\x82\xcb\x22\x18\x67\xcb\x52\xc4\x4a\x86\x9e\xa9\x1a\x1a\xdd", fs_kip_patches_600_40 },
    { "FS", "\x96\x6a\xdd\x3d\x20\xb6\x27\x13\x2c\x5a\x8d\xa4\x9a\xc9\xd8\xdd", fs_kip_patches_600_40_exfat },
    { "FS", "\x3a\x57\x4d\x43\x61\x86\x19\x1d\x17\x88\xeb\x2c\x0f\x07\x6b\x11", fs_kip_patches_600_50 },
    { "FS", "\x33\x05\x53\xf6\xb5\xfb\x55\xc4\xc2\xd7\xb7\x36\x24\x02\x76\xb3", fs_kip_patches_600_50_exfat },
    { "FS", "\x2a\xdb\xe9\x7e\x9b\x5f\x41\x77\x9e\xc9\x5f\xfe\x26\x99\xc9\x33", fs_kip_patches_700 },
    { "FS", "\x2c\xce\x65\x9c\xec\x53\x6a\x8e\x4d\x91\xf3\xbe\x4b\x74\xbe\xd3", fs_kip_patches_700_exfat },
    { "FS", "\xb2\xf5\x17\x6b\x35\x48\x36\x4d\x07\x9a\x29\xb1\x41\xa2\x3b\x06", fs_kip_patches_800 },
    { "FS", "\xdb\xd9\x41\xc0\xc5\x3c\x52\xcc\xf7\x20\x2c\x84\xd8\xe0\xf7\x80", fs_kip_patches_800_exfat },
    { "FS", "\x6b\x09\xb6\x7b\x29\xc0\x20\x24\x6d\xc3\x4f\x5a\x04\xf5\xd3\x09", fs_kip_patches_810 },
    { "FS", "\xb4\xca\xe1\xf2\x49\x65\xd9\x2e\xd2\x4e\xbe\x9e\x97\xf6\x09\xc3", fs_kip_patches_810_exfat },
    { NULL, NULL, NULL },
};

int kippatch_apply(u8 *kipdata, u64 kipdata_len, kippatch_t *patch) {
    if (!patch || !patch->diffs) return -1;

    for (kipdiff_t *diff = patch->diffs; diff->len; ++diff) {
        if (!diff->len || diff->offset + diff->len > kipdata_len)
            return 1 + (int)(diff - patch->diffs);
        u8 *start = kipdata + diff->offset;
        if (memcmp(start, diff->orig_bytes, diff->len))
            continue;
        // TODO: maybe start copying after every diff has been verified?
        memcpy(start, diff->patch_bytes, diff->len);
    }

    return 0;
}

int nca_patch(u8 * kipdata, u64 kipdata_len) {
    char pattern[8] = {0xE5, 0x07, 0x00, 0x32, 0xE0, 0x03, 0x16, 0xAA};
    char buf[0x10];
    memcpy(buf, kipdata+0x1C450, 0x10);
    u32 * addr = (u32*)memsearch(kipdata, kipdata_len, pattern, sizeof(pattern));
    int ret=0;
    int max_dist = 0x10;
    for(int i=0; i<max_dist; i++) {
        u32 op = addr[i];
        if((op & 0xFC000000)==0x94000000) { //is a BL op
            addr[i] = NOP_v8;
            ret=1;
            break;
        }
    }
    return ret;
}

int kippatch_apply_set(u8 *kipdata, u64 kipdata_len, kippatchset_t *patchset) {
    char *patchFilter[] = { "nosigchk", "nocmac", "nogc", NULL };

    if (!fopen("/ReiNX/nogc", "rb")) {
        patchFilter[2] = NULL;
        fclose();
    }

    for (kippatch_t *p = patchset->patches; p && p->name; ++p) {
        int found = 0;
        for (char **filtname = patchFilter; filtname && *filtname; ++filtname) {
            if (!strcmp(p->name, *filtname)) {
                found = 1;
                break;
            }
        }

        if (patchFilter && !found) continue;

        int r = kippatch_apply(kipdata, kipdata_len, p);
        if (r) return r;
    }
    
    return 0;
}

kippatchset_t *kippatch_find_set(u8 *kiphash, kippatchset_t *patchsets) {
    for (kippatchset_t *ps = patchsets; ps && ps->kip_name; ++ps) {
        if (!memcmp(kiphash, ps->kip_hash, 0x10)) return ps;
    }
    return NULL;
}
