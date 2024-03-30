#include <common.h>
#include <command.h>
#include <asm/addrspace.h>
#include <asm/io.h>
#include <asm/types.h>
#include "ath_nand_flash.h"
#include "ar7240_soc.h"

flash_info_t		flash_info[CFG_MAX_FLASH_BANKS];
ath_nand_flinfo_t	nf_info;
int			ath_nand_init_done = 0;

unsigned get_nand_status(void)
{
	unsigned	rddata;

	rddata = ar7240_reg_rd(AR7240_NF_RST_REG);
	while (rddata != 0xff) {
		rddata = ar7240_reg_rd(AR7240_NF_RST_REG);
	}

	ar7240_reg_wr(AR7240_NF_RST, 0x07024);  // READ STATUS
	rddata = ar7240_reg_rd(AR7240_NF_RD_STATUS);

	return rddata;
}

/*
 * globals
 */
void read_id(ath_nand_id_t *id)
{
	flush_cache(id, 8);
	ar7240_reg_wr(AR7240_NF_DMA_ADDR, (unsigned)virt_to_phys(id));
	ar7240_reg_wr(AR7240_NF_ADDR0_0, 0x0);
	ar7240_reg_wr(AR7240_NF_ADDR0_1, 0x0);
	ar7240_reg_wr(AR7240_NF_DMA_COUNT, 0x8);
	ar7240_reg_wr(AR7240_NF_PG_SIZE, 0x8);
	ar7240_reg_wr(AR7240_NF_DMA_CTRL, 0xcc);
	ar7240_reg_wr(AR7240_NF_RST, 0x9061); 	// READ ID

	get_nand_status();	// Don't check for status

	flush_cache(id, 8);

	printf(	"____ %s _____\n"
		"  vid did wc  ilp nsp ct  dp  sa1 org bs  sa0 ss  ps  res1 pls pn  res2\n"
		"0x%3x %3x %3x %3x %3x %3x %3x %3x %3x %3x %3x %3x %3x %3x  %3x %3x %3x\n"
		"-------------\n", __func__,
			id->vid, id->did, id->wc, id->ilp,
			id->nsp, id->ct, id->dp, id->sa1,
			id->org, id->bs, id->sa0, id->ss,
			id->ps, id->res1, id->pls, id->pn,
			id->res2);
}

void flash_print_info (flash_info_t *info)
{
	ath_nand_id_t	id;
	read_id(&id);
}


void nand_block_erase(unsigned addr0, unsigned addr1)
{
	unsigned	rddata;

	//printf("NAND FLASH BLOCK ERASE - 0x%x%x\n", addr1, addr0);

	ar7240_reg_wr(AR7240_NF_ADDR0_0, addr0);
	ar7240_reg_wr(AR7240_NF_ADDR0_1, addr1);
	ar7240_reg_wr(AR7240_NF_RST, 0xd0600e);	// BLOCK ERASE

	rddata = (get_nand_status() & AR7240_NF_READ_STATUS_MASK);
	if (rddata != AR7240_NF_STATUS_OK) {
		printf("Erase Failed. status = 0x%x", rddata);
	}
}


int
flash_erase(flash_info_t *info, int s_first, int s_last)
{
	int i, j;

	printf("\nFirst %#x last %#x sector size %#x\n",
		s_first, s_last, nf_info.erasesize);

	for (j = 0, i = s_first; i <= s_last; j++, i += nf_info.erasesize) {
		ulong ba0, ba1, b, p;

		b = i >> nf_info.erasesize_shift;
		p = (i & nf_info.erasesize_mask) >> nf_info.writesize_shift;

		ba0 = (b << 22) | (p << 16);
		ba1 = (b >>  9) & 0xf;
#if 1
		printf("\b\b\b\b%4d", j);
#else
		printf("flash_erase: 0x%x 0x%x%08x\n", i, ba1, ba0);
#endif
		nand_block_erase(ba0, ba1);

	}

	printf("\n");

	return 0;
}

unsigned
rw_page(int rd, unsigned addr0, unsigned addr1, unsigned count, unsigned *buf)
{
	unsigned	rddata;

	ar7240_reg_wr(AR7240_NF_ADDR0_0, addr0);
	ar7240_reg_wr(AR7240_NF_ADDR0_1, addr1);
	ar7240_reg_wr(AR7240_NF_DMA_ADDR, (unsigned)buf);
	ar7240_reg_wr(AR7240_NF_DMA_COUNT, count);
	ar7240_reg_wr(AR7240_NF_PG_SIZE, count);

	if (rd) {	// Read Page
		ar7240_reg_wr(AR7240_NF_DMA_CTRL, 0xcc);
		ar7240_reg_wr(AR7240_NF_RST, 0x30006a);
	} else {	// Write Page
		ar7240_reg_wr(AR7240_NF_DMA_CTRL, 0x8c);
		ar7240_reg_wr(AR7240_NF_RST, 0x10804c);
	}

	rddata = (get_nand_status() & AR7240_NF_READ_STATUS_MASK);
	if (rddata != AR7240_NF_STATUS_OK) {
		(rd == 1) ? printf("Read") : printf("Write");
		printf(" Failed. status = 0x%x\n", rddata);
	}

	return rddata;
}

/* max page and spare size */
uchar partial_read_buffer[4096 + 256];

int
rw_buff(int rd, uchar *buf, ulong addr, ulong len, unsigned pgsz)
{
	const unsigned	nbs = 65536;
	ulong		total = 0;
	unsigned	ret = AR7240_NF_STATUS_OK,
			dbs = nf_info.writesize;
	uchar		*ptr;

	if (ath_nand_init_done != 'WASP') {
		/*
		 * env_init() tries to read the flash. However, it is
		 * called before flash_init. Hence read with defaults
		 */
		nf_info.erasesize_shift = AR7240_NF_BLK_SIZE_S;
		nf_info.erasesize_mask = AR7240_NF_BLK_SIZE_M;
		dbs = pgsz = nf_info.writesize = 2048;
	}

	while (total < len && ret == AR7240_NF_STATUS_OK) {
		ulong ba0, ba1, b, p;

		b = addr >> nf_info.erasesize_shift;
		p = (addr & nf_info.erasesize_mask) >> nf_info.writesize_shift;

		ba0 = (b << 22) | (p << 16);
		ba1 = (b >>  9) & 0xf;

		ptr = buf;

		if ((len - total) < pgsz) {
			ptr = partial_read_buffer;
			if (!rd) {
				memcpy(ptr, buf, len - total);
				printf("memcpy(%p, %p, %d)\n", ptr, buf, len-total);
			}
		}

		//if (!rd) {
		//	printf("rw_buff(%d): 0x%x 0x%x%08x buf = %p pgsz %u\n",
		//			rd, addr, ba1, ba0, ptr, pgsz);
		//}

		flush_cache(ptr, pgsz);	// for writes...
		ret = rw_page(rd, ba0, ba1, pgsz, virt_to_phys(ptr));
		flush_cache(ptr, pgsz);	// for read ???

		total += pgsz;
		buf += pgsz;
		addr += dbs;
	}

	if (rd && ptr == partial_read_buffer) {
		buf -= dbs;
		total -= dbs;
		memcpy(buf, ptr, len - total);
	}

	return (ret != AR7240_NF_STATUS_OK);
}

int
write_buff(flash_info_t *info, uchar *buf, ulong addr, ulong len)
{
	if (info) {
		return rw_buff(0 /* write */, buf, addr, len, nf_info.writesize + nf_info.oobsize);
	} else {
		return rw_buff(0 /* write */, buf, addr, len, nf_info.writesize);
	}
}

int
read_buff(flash_info_t *info, uchar *buf, ulong addr, ulong len)
{
	if (info) {
		return rw_buff(1 /* read */, buf, addr, len, nf_info.writesize + nf_info.oobsize);
	} else {
		return rw_buff(1 /* read */, buf, addr, len, nf_info.writesize);
	}
}


unsigned long
flash_init(void)
{
	uint64_t ath_plane_size[] =
		{ 64 << 20, 1 << 30, 2 << 30, 4 << 30, 8 << 30 };
	ath_nand_id_t		*id;

        // Control Reg Setting
	ar7240_reg_wr(AR7240_NF_CTRL,	(AR7240_NF_ADDR_CYCLES_NUM) |
					(AR7240_NF_BLK_SIZE << 6) |
					(AR7240_NF_PAGE_SIZE << 8) |
					(AR7240_NF_CUSTOM_SIZE_EN << 11));

	// TIMINGS_ASYN Reg Settings
	ar7240_reg_wr(AR7240_NF_TIMINGS_ASYN, AR7240_NF_TIMING_ASYN);

	// NAND Mem Control Reg
	ar7240_reg_wr(AR7240_NF_MEM_CTRL, 0xff00);

	// Reset Command
	ar7240_reg_wr(AR7240_NF_RST, 0xff00);

	id = &nf_info.id;
	read_id(id);

	nf_info.size		= ath_plane_size[id->pls] << id->pn;

	nf_info.writesize_shift	= 10 + id->ps;
	nf_info.writesize	= (1024 << id->ps);
	nf_info.writesize_mask	= (nf_info.writesize - 1);

	nf_info.erasesize_shift	= (16 + id->bs);
	nf_info.erasesize	= (1 << nf_info.erasesize_shift);
	nf_info.erasesize_mask	= (nf_info.erasesize - 1);

	nf_info.oobsize		= (nf_info.writesize / 512) * (8 << id->ss);

	ath_nand_init_done = 'WASP';

	/*
	 * hook into board specific code to fill flash_info
	 */
	printf("*******NAND Flash Init is done******\n");

	return (flash_get_geom(&flash_info[0]));
}
