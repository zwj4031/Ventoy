
FILE_LICENCE ( GPL2_OR_LATER_OR_UBDL );

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <byteswap.h>
#include <errno.h>
#include <assert.h>
#include <ipxe/blockdev.h>
#include <ipxe/io.h>
#include <ipxe/acpi.h>
#include <ipxe/sanboot.h>
#include <ipxe/device.h>
#include <ipxe/pci.h>
#include <ipxe/eltorito.h>
#include <ipxe/timer.h>
#include <ipxe/umalloc.h>
#include <realmode.h>
#include <bios.h>
#include <biosint.h>
#include <bootsector.h>
#include <int13.h>
#include <ventoy.h>

int g_debug = 0;
char *g_cmdline_copy;
void *g_initrd_addr;
size_t g_initrd_len;
ventoy_chain_head *g_chain;
ventoy_img_chunk *g_chunk;
uint32_t g_img_chunk_num;
ventoy_img_chunk *g_cur_chunk;
uint32_t g_disk_sector_size;

ventoy_override_chunk *g_override_chunk;
uint32_t g_override_chunk_num;

ventoy_virt_chunk *g_virt_chunk;
uint32_t g_virt_chunk_num;

ventoy_sector_flag g_sector_flag[128];

static struct int13_disk_address __bss16 ( ventoy_address );
#define ventoy_address __use_data16 ( ventoy_address )

static uint64_t ventoy_remap_lba(uint64_t lba, uint32_t *count)
{
    uint32_t i;
    uint32_t max_sectors;
    ventoy_img_chunk *cur;

    if ((NULL == g_cur_chunk) || ((lba) < g_cur_chunk->img_start_sector) || ((lba) > g_cur_chunk->img_end_sector))
    {
        g_cur_chunk = NULL;
        for (i = 0; i < g_img_chunk_num; i++)
        {
            cur = g_chunk + i;
            if (lba >= cur->img_start_sector && lba <= cur->img_end_sector)
            {
                g_cur_chunk = cur;
                break;
            }
        }
    }

    if (g_cur_chunk)
    {
        max_sectors = g_cur_chunk->img_end_sector - lba + 1;
        if (*count > max_sectors)
        {
            *count = max_sectors;
        }

        if (512 == g_disk_sector_size)
        {
            return g_cur_chunk->disk_start_sector + ((lba - g_cur_chunk->img_start_sector) << 2);            
        }
        return g_cur_chunk->disk_start_sector + (lba - g_cur_chunk->img_start_sector) * 2048 / g_disk_sector_size;
    }
    return lba;
}

static int ventoy_vdisk_read_real(uint64_t lba, unsigned int count, unsigned long buffer)
{
    uint32_t i = 0;
    uint32_t left = 0;
    uint32_t readcount = 0;
    uint32_t tmpcount = 0;
    uint16_t status = 0;
    uint64_t curlba = 0;
    uint64_t maplba = 0;
    uint64_t start = 0;
    uint64_t end = 0;
    uint64_t override_start = 0;
    uint64_t override_end = 0;
    unsigned long phyaddr;
    unsigned long databuffer = buffer;
    uint8_t *override_data;

    curlba = lba;
    left = count;

    while (left > 0)
    {
        readcount = left;
        maplba = ventoy_remap_lba(curlba, &readcount);
        
        if (g_disk_sector_size == 512)
        {
            tmpcount = (readcount << 2);
        }
        else
        {
            tmpcount = (readcount * 2048) / g_disk_sector_size;
        }

        phyaddr = user_to_phys(buffer, 0);

        while (tmpcount > 0)
        {
            /* Use INT 13, 42 to read the data from real disk */
            ventoy_address.lba = maplba;
            ventoy_address.buffer.segment = (uint16_t)(phyaddr >> 4);
    	    ventoy_address.buffer.offset = (uint16_t)(phyaddr & 0x0F);

            if (tmpcount >= 64) /* max sectors per transmit */
            {
                ventoy_address.count = 64;
                tmpcount -= 64;
                maplba   += 64;
                phyaddr  += 32768;
            }
            else
            {
                ventoy_address.count = tmpcount;
                tmpcount = 0;
            }

            __asm__ __volatile__ ( REAL_CODE ( "stc\n\t"
    					   "sti\n\t"
    					   "int $0x13\n\t"
    					   "sti\n\t" /* BIOS bugs */
    					   "jc 1f\n\t"
    					   "xorw %%ax, %%ax\n\t"
    					   "\n1:\n\t" )
    			       : "=a" ( status )
    			       : "a" ( 0x4200 ), "d" ( VENTOY_BIOS_FAKE_DRIVE ),
    				 "S" ( __from_data16 ( &ventoy_address ) ) );
        }

        curlba += readcount;
        left -= readcount;
        buffer += (readcount * 2048);
    }

    start = lba * 2048;
    if (start > g_chain->real_img_size_in_bytes)
    {
        goto end;
    }

    end = start + count * 2048;
    for (i = 0; i < g_override_chunk_num; i++)
    {
        override_data = g_override_chunk[i].override_data;
        override_start = g_override_chunk[i].img_offset;
        override_end = override_start + g_override_chunk[i].override_size;

        if (end <= override_start || start >= override_end)
        {
            continue;
        }

        if (start <= override_start)
        {
            if (end <= override_end)
            {
                memcpy((char *)databuffer + override_start - start, override_data, end - override_start);  
            }
            else
            {
                memcpy((char *)databuffer + override_start - start, override_data, override_end - override_start);
            }
        }
        else
        {
            if (end <= override_end)
            {
                memcpy((char *)databuffer, override_data + start - override_start, end - start);     
            }
            else
            {
                memcpy((char *)databuffer, override_data + start - override_start, override_end - start);
            }
        }
    }

end:

    return 0;
}

int ventoy_vdisk_read(struct san_device *sandev, uint64_t lba, unsigned int count, unsigned long buffer)
{
    uint32_t i, j;
    uint64_t curlba;
    uint64_t lastlba = 0;
    uint32_t lbacount = 0;
    unsigned long lastbuffer;
    uint64_t readend;
    ventoy_virt_chunk *node;
    ventoy_sector_flag *cur_flag;
    ventoy_sector_flag *sector_flag = g_sector_flag;
    struct i386_all_regs *ix86;

    if (INT13_EXTENDED_READ != sandev->int13_command)
    {
        DBGC(sandev, "invalid cmd %u\n", sandev->int13_command);
        return 0;
    }

    ix86 = (struct i386_all_regs *)sandev->x86_regptr;

    readend = (lba + count) * 2048;
    if (readend < g_chain->real_img_size_in_bytes)
    {
        ventoy_vdisk_read_real(lba, count, buffer);
        ix86->regs.dl = sandev->drive;
        return 0;
    }

    if (count > sizeof(g_sector_flag))
    {
        sector_flag = (ventoy_sector_flag *)malloc(count * sizeof(ventoy_sector_flag));
    }

    for (curlba = lba, cur_flag = sector_flag, j = 0; j < count; j++, curlba++, cur_flag++)
    {
        cur_flag->flag = 0;
        for (node = g_virt_chunk, i = 0; i < g_virt_chunk_num; i++, node++)
        {
            if (curlba >= node->mem_sector_start && curlba < node->mem_sector_end)
            {
                memcpy((void *)(buffer + j * 2048), 
                       (char *)g_virt_chunk + node->mem_sector_offset + (curlba - node->mem_sector_start) * 2048,
                       2048);
                cur_flag->flag = 1;
                break;
            }
            else if (curlba >= node->remap_sector_start && curlba < node->remap_sector_end)
            {
                cur_flag->remap_lba = node->org_sector_start + curlba - node->remap_sector_start;
                cur_flag->flag = 2;
                break;
            }
        }
    }

    for (curlba = lba, cur_flag = sector_flag, j = 0; j < count; j++, curlba++, cur_flag++)
    {
        if (cur_flag->flag == 2)
        {
            if (lastlba == 0)
            {
                lastbuffer = buffer + j * 2048;
                lastlba = cur_flag->remap_lba;
                lbacount = 1;
            }
            else if (lastlba + lbacount == cur_flag->remap_lba)
            {
                lbacount++;
            }
            else
            {
                ventoy_vdisk_read_real(lastlba, lbacount, lastbuffer);
                lastbuffer = buffer + j * 2048;
                lastlba = cur_flag->remap_lba;
                lbacount = 1;
            }
        }
    }

    if (lbacount > 0)
    {
        ventoy_vdisk_read_real(lastlba, lbacount, lastbuffer);
    }

    if (sector_flag != g_sector_flag)
    {
        free(sector_flag);
    }

    ix86->regs.dl = sandev->drive;
    return 0;
}

static void ventoy_dump_img_chunk(ventoy_chain_head *chain)
{
    uint32_t i;
    ventoy_img_chunk *chunk;

    chunk = (ventoy_img_chunk *)((char *)chain + chain->img_chunk_offset);

    printf("##################### ventoy_dump_img_chunk #######################\n");

    for (i = 0; i < chain->img_chunk_num; i++)
    {
        printf("%2u: [ %u - %u ] <==> [ %llu - %llu ]\n",
               i, chunk[i].img_start_sector, chunk[i].img_end_sector, 
               chunk[i].disk_start_sector, chunk[i].disk_end_sector);
    }
    
    ventoy_debug_pause();
}

static void ventoy_dump_override_chunk(ventoy_chain_head *chain)
{
    uint32_t i;
    ventoy_override_chunk *chunk;
    
    chunk = (ventoy_override_chunk *)((char *)chain + chain->override_chunk_offset);

    printf("##################### ventoy_dump_override_chunk #######################\n");

    for (i = 0; i < g_override_chunk_num; i++)
    {
        printf("%2u: [ %llu, %u ]\n", i, chunk[i].img_offset, chunk[i].override_size);
    }
    
    ventoy_debug_pause();
}

static void ventoy_dump_virt_chunk(ventoy_chain_head *chain)
{
    uint32_t i;
    ventoy_virt_chunk *node;
     
    printf("##################### ventoy_dump_virt_chunk #######################\n");
    printf("virt_chunk_offset=%u\n", chain->virt_chunk_offset);
    printf("virt_chunk_num=%u\n",    chain->virt_chunk_num);

    node = (ventoy_virt_chunk *)((char *)chain + chain->virt_chunk_offset);
    for (i = 0; i < chain->virt_chunk_num; i++, node++)
    {
        printf("%2u: mem:[ %u, %u, %u ]  remap:[ %u, %u, %u ]\n", i, 
               node->mem_sector_start,
               node->mem_sector_end,
               node->mem_sector_offset,
               node->remap_sector_start,
               node->remap_sector_end,
               node->org_sector_start);
    }
    
    ventoy_debug_pause();
}

static void ventoy_dump_chain(ventoy_chain_head *chain)
{
    uint32_t i = 0;
    uint8_t chksum = 0;
    uint8_t *guid;
    
    guid = chain->os_param.vtoy_disk_guid;
    for (i = 0; i < sizeof(ventoy_os_param); i++)
    {
        chksum += *((uint8_t *)(&(chain->os_param)) + i);
    }

    printf("##################### ventoy_dump_chain #######################\n");

    printf("os_param will be save at %p\n", ventoy_get_runtime_addr());

    printf("os_param->chksum=0x%x (%s)\n", chain->os_param.chksum, chksum ? "FAILED" : "SUCCESS");
    printf("os_param->vtoy_disk_guid=%02x%02x%02x%02x\n", guid[0], guid[1], guid[2], guid[3]);
    printf("os_param->vtoy_disk_size=%llu\n",     chain->os_param.vtoy_disk_size);
    printf("os_param->vtoy_disk_part_id=%u\n",    chain->os_param.vtoy_disk_part_id);
    printf("os_param->vtoy_disk_part_type=%u\n",  chain->os_param.vtoy_disk_part_type);
    printf("os_param->vtoy_img_path=<%s>\n",      chain->os_param.vtoy_img_path);
    printf("os_param->vtoy_img_size=<%llu>\n",    chain->os_param.vtoy_img_size);
    printf("os_param->vtoy_img_location_addr=<0x%llx>\n", chain->os_param.vtoy_img_location_addr);
    printf("os_param->vtoy_img_location_len=<%u>\n",   chain->os_param.vtoy_img_location_len);
    ventoy_debug_pause();

    printf("chain->disk_drive=0x%x\n",          chain->disk_drive);
    printf("chain->drive_map=0x%x\n",           chain->drive_map);
    printf("chain->disk_sector_size=%u\n",      chain->disk_sector_size);
    printf("chain->real_img_size_in_bytes=%llu\n",   chain->real_img_size_in_bytes);
    printf("chain->virt_img_size_in_bytes=%llu\n", chain->virt_img_size_in_bytes);
    printf("chain->boot_catalog=%u\n",          chain->boot_catalog);
    printf("chain->img_chunk_offset=%u\n",      chain->img_chunk_offset);
    printf("chain->img_chunk_num=%u\n",         chain->img_chunk_num);
    printf("chain->override_chunk_offset=%u\n", chain->override_chunk_offset);
    printf("chain->override_chunk_num=%u\n",    chain->override_chunk_num);
    printf("chain->virt_chunk_offset=%u\n",    chain->virt_chunk_offset);
    printf("chain->virt_chunk_num=%u\n",    chain->virt_chunk_num);
    ventoy_debug_pause();

    ventoy_dump_img_chunk(chain);
    ventoy_dump_override_chunk(chain);
    ventoy_dump_virt_chunk(chain);
}

static int ventoy_update_image_location(ventoy_os_param *param)
{
    uint8_t chksum = 0;
    unsigned int i;
    unsigned int length;
    userptr_t address = 0;
    ventoy_image_location *location = NULL;
    ventoy_image_disk_region *region = NULL;
    ventoy_img_chunk *chunk = g_chunk;

    length = sizeof(ventoy_image_location) + (g_img_chunk_num - 1) * sizeof(ventoy_image_disk_region);

    address = umalloc(length + 4096 * 2);
    if (!address)
    {
        return 0;
    }

    if (address % 4096)
    {
        address += 4096 - (address % 4096);
    }

    param->chksum = 0;
    param->vtoy_img_location_addr = user_to_phys(address, 0);
    param->vtoy_img_location_len = length;

    /* update check sum */
    for (i = 0; i < sizeof(ventoy_os_param); i++)
    {
        chksum += *((uint8_t *)param + i);
    }
    param->chksum = (chksum == 0) ? 0 : (uint8_t)(0x100 - chksum);

    location = (ventoy_image_location *)(unsigned long)(address);
    if (NULL == location)
    {
        return 0;
    }
    
    memcpy(&location->guid, &param->guid, sizeof(ventoy_guid));
    location->image_sector_size = 2048;
    location->disk_sector_size  = g_chain->disk_sector_size;
    location->region_count = g_img_chunk_num;

    region = location->regions;

    for (i = 0; i < g_img_chunk_num; i++)
    {
        region->image_sector_count = chunk->img_end_sector - chunk->img_start_sector + 1;
        region->image_start_sector = chunk->img_start_sector;
        region->disk_start_sector  = chunk->disk_start_sector;
        region++;
        chunk++;
    }

    return 0;
}

int ventoy_boot_vdisk(void *data)
{
    uint8_t chksum = 0;
    unsigned int i;
    unsigned int drive;
    
    (void)data;

    ventoy_address.bufsize = offsetof ( typeof ( ventoy_address ), buffer_phys );

    if (strstr(g_cmdline_copy, "debug"))
    {
        g_debug = 1;
        printf("### ventoy chain boot begin... ###\n");
        ventoy_debug_pause();
    }

    g_chain = (ventoy_chain_head *)g_initrd_addr;
    g_chunk = (ventoy_img_chunk *)((char *)g_chain + g_chain->img_chunk_offset);
    g_img_chunk_num = g_chain->img_chunk_num;
    g_disk_sector_size = g_chain->disk_sector_size;
    g_cur_chunk = g_chunk;

    g_override_chunk = (ventoy_override_chunk *)((char *)g_chain + g_chain->override_chunk_offset);
    g_override_chunk_num = g_chain->override_chunk_num;

    g_virt_chunk = (ventoy_virt_chunk *)((char *)g_chain + g_chain->virt_chunk_offset);
    g_virt_chunk_num = g_chain->virt_chunk_num;

    if (g_debug)
    {
        for (i = 0; i < sizeof(ventoy_os_param); i++)
        {
            chksum += *((uint8_t *)(&(g_chain->os_param)) + i);
        }
        printf("os param checksum: 0x%x %s\n", g_chain->os_param.chksum, chksum ? "FAILED" : "SUCCESS");
    }

    ventoy_update_image_location(&(g_chain->os_param));

    if (g_debug)
    {
        ventoy_dump_chain(g_chain);
    }

    drive = ventoy_int13_hook(g_chain);

    if (g_debug)
    {
        printf("### ventoy chain boot before boot image ... ###\n");
        ventoy_debug_pause();    
    }
    
    ventoy_int13_boot(drive, &(g_chain->os_param), g_cmdline_copy);

    if (g_debug)
    {
        printf("!!!!!!!!!! ventoy boot failed !!!!!!!!!!\n");
        ventoy_debug_pause();
    }

    return 0;
}

