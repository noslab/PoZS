// SPDX-License-Identifier: GPL-2.0-only

#include "nvmev.h"
#include "ssd.h"
#include "zns_ftl.h"

#include <linux/ktime.h>
#include <linux/sched/clock.h>

#define MAX_STREAMS 6
//#define TIME_WINDOW_NS (10 * 1000000000ULL)
#define TIME_WINDOW_NS (60 * 1000000000ULL)

//#define TRAFFIC_TH 7516192768 // 1073741824 * 42 / 6 (ZONE_COUNT / 3 * 2)
#define TRAFFIC_TH 15032385536
#define MAX_CELL 7
//#define TRAFFIC_TH 1073741824 * MAX_CELL * 2 / 3

uint32_t get_action(uint32_t stream, uint32_t zid, struct zns_ftl *zns_ftl);

static inline uint32_t __nr_lbas_from_rw_cmd(struct nvme_rw_command *cmd)
{
	return cmd->length + 1;
}

static bool __check_boundary_error(struct zns_ftl *zns_ftl, uint64_t slba, uint32_t nr_lba)
{
	return lba_to_zone(zns_ftl, slba) == lba_to_zone(zns_ftl, slba + nr_lba - 1);
}

static void __increase_write_ptr(struct zns_ftl *zns_ftl, uint32_t zid, uint32_t nr_lba)
{
	struct zone_descriptor *zone_descs = zns_ftl->zone_descs;
	uint64_t cur_write_ptr = zone_descs[zid].wp;
	uint64_t zone_capacity = zone_descs[zid].zone_capacity;

	cur_write_ptr += nr_lba;

	zone_descs[zid].wp = cur_write_ptr;
	if (cur_write_ptr == zone_to_slba(zns_ftl, zid)) {
		switch (zid % 3) {
			case 0:
				zns_ftl->LSB_u++;
				break;
			case 1:
				zns_ftl->MSB_u++;
				break;
			case 2:
				zns_ftl->CSB_u++;
				break;
		}
	}

	if (cur_write_ptr == (zone_to_slba(zns_ftl, zid) + zone_capacity)) {
		//change state to ZSF
		release_zone_resource(zns_ftl, OPEN_ZONE);
		release_zone_resource(zns_ftl, ACTIVE_ZONE);

		// [CY]
		CY_DEBUG_PRINT("%d zone FULL\n", zid);

		zns_ftl->ZT_list[zid] = 0;
		zns_ftl->RU_list[zid] = 0;

		switch (zid % 3) {
			case 0: { // LSB
				zns_ftl->LSB_f++;
				break;
			}
			case 2: { // CSB
				zns_ftl->CSB_f++;
				break;
			}
			case 1: { // MSB
				zns_ftl->MSB_f++;
				break;
			}
		}
		//


		if (zone_descs[zid].zrwav)
			ASSERT(0);

		change_zone_state(zns_ftl, zid, ZONE_STATE_FULL);
	} else if (cur_write_ptr > (zone_to_slba(zns_ftl, zid) + zone_capacity)) {
		NVMEV_ERROR("[%s] Write Boundary error!!\n", __func__);
	}
}

static inline struct ppa __lpn_to_ppa(struct zns_ftl *zns_ftl, uint64_t lpn)
{
	struct ssdparams *spp = &zns_ftl->ssd->sp;
	struct znsparams *zpp = &zns_ftl->zp;
	uint64_t zone = lpn_to_zone(zns_ftl, lpn); // find corresponding zone
	uint64_t off = lpn - zone_to_slpn(zns_ftl, zone);

	uint32_t sdie = (zone * zpp->dies_per_zone) % spp->tt_luns;
	uint32_t die = sdie + ((off / spp->pgs_per_oneshotpg) % zpp->dies_per_zone);

	uint32_t channel = die_to_channel(zns_ftl, die);
	uint32_t lun = die_to_lun(zns_ftl, die);
	struct ppa ppa = {
		.g = {
			.lun = lun,
			.ch = channel,
			.pg = off % spp->pgs_per_oneshotpg,
		},
	};

	return ppa;
}

// [CY]
void record_write(struct zns_ftl *zns_ftl, uint64_t len)
{
	uint32_t write_stream, i;
	uint32_t stream = zns_ftl->stream;
	uint64_t current_time = ktime_get_ns();

	if (current_time - zns_ftl->last_update_time > TIME_WINDOW_NS) {
		if (stream < MAX_STREAMS - 1) {
			stream++;
			zns_ftl->write_traffic[stream] = 0;
			zns_ftl->write_traffic[stream] += zns_ftl->write_traffic[zns_ftl->stream] / 4 * 3;
		} else {
			stream = 0;
			zns_ftl->write_traffic[stream] = 0;
			zns_ftl->write_traffic[stream] += zns_ftl->write_traffic[MAX_STREAMS] / 4 * 3;
			for (i=1; i<MAX_STREAMS; i++)
				zns_ftl->write_traffic[stream] = 0;
		}
	}

	zns_ftl->stream = stream;

	zns_ftl->write_traffic[stream] += len;
	CY_DEBUG_PRINT("write_traffic[%d] : %lld\n", stream, zns_ftl->write_traffic[stream]);

	zns_ftl->last_update_time = current_time;
}

// [CY]
uint32_t check_zid(uint32_t zid, struct zns_ftl *zns_ftl, uint32_t action, uint64_t nr_lba)
{
	struct zone_descriptor *zone_descs = zns_ftl->zone_descs;

	if (action < 0 || action > 2) {
		CY_DEBUG_PRINT("cehck_action_err : %d\n", action);
		action = 0;
	}

	if (zone_descs[zid].wp + nr_lba <= zone_descs[zid].zone_capacity * (zid + 1))
		return zid;

	switch(action) {
		case 0: {
			if (zns_ftl->LSB_f >= MAX_CELL) {
				if (zns_ftl->CSB_f < MAX_CELL) action = 2;
				else action = 1;
			}
			break;
		}
		case 1: {
			if (zns_ftl->MSB_f >= MAX_CELL) {
				if (zns_ftl->LSB_f < MAX_CELL) action = 0;
				else action = 2;
			}
			break;
		}
		case 2: {
			if (zns_ftl->CSB_f >= MAX_CELL) {
				if (zns_ftl->MSB_f < MAX_CELL) action = 1;
				else action = 0;
			}
			break;
		}
	}

	while (zid % 3 != action) {
		zid++;
		if (zone_descs[zid].wp + nr_lba > zone_descs[zid].zone_capacity * (zid + 1) || zns_ftl->reset_req_list[zid] == 1) {
			zid++;
		}
		
	}
	if (zid > 41) {
		zid = 0;
		while (zid % 3 != 2 && zns_ftl->CSB_f < MAX_CELL) { // CSB
			zid++;
			if (zone_descs[zid].wp + nr_lba > zone_descs[zid].zone_capacity * (zid + 1) || zns_ftl->reset_req_list[zid] == 1) {
				zid++;
			}
		}
	}
	if (zid > 41) {
		zid = 0;
		while (zid % 3 != 1 && zns_ftl->MSB_f < MAX_CELL) { // MSB
			zid++;
			if (zone_descs[zid].wp + nr_lba > zone_descs[zid].zone_capacity * (zid + 1) || zns_ftl->reset_req_list[zid] == 1) {
				zid++;
			}
		}
	}
	if (zid > 41) {
		zid = 0;
		while (zid % 3 != 0) { // LSB
			zid++;
			if (zone_descs[zid].wp + nr_lba > zone_descs[zid].zone_capacity * (zid + 1) || zns_ftl->reset_req_list[zid] == 1) {
				zid++;
			}
		}
	}

	return zid;
}

//[CY]
uint32_t Q_zid(uint32_t zid, struct zns_ftl *zns_ftl, uint32_t action, uint64_t nr_lba)
{
	uint32_t new_zid;

	switch (action) {
		case 1: {
			if (zns_ftl->LSB_u < zns_ftl->MSB_u + 1)
				action = 0;
			else if (zns_ftl->CSB_u < zns_ftl->MSB_u + 1)
				action = 2;
			break;
		}
		case 2: {
			if (zns_ftl->LSB_u < zns_ftl->CSB_u && zns_ftl->LSB_u != MAX_CELL)
				action = 0;
			break;
		}
	}

	switch(action) {
		case 0: { // LSB
			if (zns_ftl->LSB_u >= MAX_CELL) {
				action = 2;
				Q_zid(zid, zns_ftl, action, nr_lba);
			} else new_zid = check_zid(zid, zns_ftl, action, nr_lba);
			break;
		}
		case 1: { // MSB
			if (zns_ftl->MSB_u >= MAX_CELL) {
				action = 0;
				Q_zid(zid, zns_ftl, action, nr_lba);
			} else new_zid = check_zid(zid, zns_ftl, action, nr_lba);
			break;			
		}
		case 2: { // CSB
			if (zns_ftl->CSB_u >= MAX_CELL) {
				action = 1;
				Q_zid(zid, zns_ftl, action, nr_lba);
			} else new_zid = check_zid(zid, zns_ftl, action, nr_lba);
			break;		
		}
	}

	return new_zid;
}

uint32_t RU_write(struct zns_ftl *zns_ftl)
{
	uint32_t action;

	if (zns_ftl->LSB_f == 0)
		action = 0;
	else if (zns_ftl->CSB_f == 0)
		action = 2;
	else
		action = 1;

	return action;
}

uint64_t update_time=0;

static bool __zns_write(struct zns_ftl *zns_ftl, struct nvmev_request *req,
			struct nvmev_result *ret)
{
	struct zone_descriptor *zone_descs = zns_ftl->zone_descs;
	struct ssdparams *spp = &zns_ftl->ssd->sp;
	struct nvme_rw_command *cmd = &(req->cmd->rw);

	uint64_t slba = cmd->slba;
	uint64_t nr_lba = __nr_lbas_from_rw_cmd(cmd);
	uint64_t slpn, elpn, lpn, zone_elpn;
	// get zone from start_lba
	uint32_t zid = lba_to_zone(zns_ftl, slba);
	enum zone_state state = zone_descs[zid].state;

	uint64_t nsecs_start = req->nsecs_start;
	uint64_t nsecs_xfer_completed = nsecs_start;
	uint64_t nsecs_latest = nsecs_start;
	uint32_t status = NVME_SC_SUCCESS;

	uint64_t pgs = 0;

	struct buffer *write_buffer;

	uint32_t stream, L_mode, new_zid, action, RU_action;
	uint64_t len;

	uint64_t cur_time = ktime_get_ns();

	// [CY]
	uint64_t remap_s = 0;
	uint64_t remap_e = 0;
	uint64_t cy_gc = 0;
	uint64_t cy_gc_e = 0;

/* [CY]
	zid % 3 으로 LSB, CSB, MSB 존 구분
	0: LSB / 2: CSB / 1: MSB
*/
	// [CY]
	slpn = lba_to_lpn(zns_ftl, slba);
	elpn = lba_to_lpn(zns_ftl, slba + nr_lba - 1);
	len = elpn-slpn;
	record_write(zns_ftl, len);
	stream = zns_ftl->stream;

	//[CY_remap]
	remap_s = local_clock();

	if (zns_ftl->write_traffic[stream] < TRAFFIC_TH && zns_ftl->LSB_f < (MAX_CELL/2)) {
		L_mode = 1;
	} else {
		L_mode = 0;
	}

	if (zns_ftl->waiting_reset >= (MAX_CELL/3*2)) {
		RU_action = 3;
	}

	// 설정된 존번호로 재설정
	if (zns_ftl->ZT_list[zid] != 0) {
		action = (zns_ftl->ZT_list[zid] - 1) % 3;
		zid = zns_ftl->ZT_list[zid] - 1;

		new_zid = check_zid(zid, zns_ftl, action, nr_lba);
		if (zid != new_zid)
			zns_ftl->ZT_list[zid] = new_zid + 1;
	}
	// LSB만 사용
	else if (zns_ftl->ZT_list[zid]==0 && L_mode==1 && slba==(zone_to_slba(zns_ftl, zid) * zid)) {
		new_zid = check_zid(zid, zns_ftl, 0, nr_lba);
		zns_ftl->ZT_list[zid] = (new_zid + 1);
		CY_DEBUG_PRINT("L_mode : %d -> %d\n", zid, new_zid);
	}

	else if (slba == (zone_to_slba(zns_ftl, zid) * zid) && L_mode==0) {
		action = get_action(stream, zid, zns_ftl);
		if (action == 3 || RU_action == 3) {
			RU_action = 3;
			action = RU_write(zns_ftl);
		}
		new_zid = Q_zid(zid, zns_ftl, action, nr_lba);
		zns_ftl->ZT_list[zid] = (new_zid + 1);
		if (RU_action == 3)
			zns_ftl->RU_list[zid] = 1;
	}
	// check zone capacity
	else {
		if (L_mode == 1) {
			new_zid = check_zid(zid, zns_ftl, 0, nr_lba);
			zns_ftl->ZT_list[zid] = (new_zid + 1);
		}
		if (slba == (zone_to_slba(zns_ftl, zid) * zid)) {
			action = get_action(stream, zid, zns_ftl);
			new_zid = Q_zid(zid, zns_ftl, action, nr_lba);
			zns_ftl->ZT_list[zid] = (new_zid + 1);
		}
	}

	zid = new_zid;
	slba = zone_descs[zid].wp;
	cmd->slba = slba;
	state = zone_descs[zid].state;
	action = zid % 3;

	//[CY_remap]
	remap_e = local_clock();
	cy_remap += (remap_e - remap_s);

	////////////////////////////////////////////////////////////////////////////

	if (cmd->opcode == nvme_cmd_zone_append || slba != zone_descs[zid].wp) {
		slba = zone_descs[zid].wp;
		cmd->slba = slba;
	}

	slpn = lba_to_lpn(zns_ftl, slba);
	elpn = lba_to_lpn(zns_ftl, slba + nr_lba - 1);
	zone_elpn = zone_to_elpn(zns_ftl, zid);

	NVMEV_ZNS_DEBUG("%s slba 0x%llx nr_lba 0x%lx zone_id %d state %d\n", __func__, slba,
			nr_lba, zid, state);

	if (zns_ftl->zp.zone_wb_size)
		write_buffer = &(zns_ftl->zone_write_buffer[zid]);
	else
		write_buffer = zns_ftl->ssd->write_buffer;

	if (buffer_allocate(write_buffer, LBA_TO_BYTE(nr_lba)) < LBA_TO_BYTE(nr_lba))
		return false;

	if ((LBA_TO_BYTE(nr_lba) % spp->write_unit_size) != 0) {
		status = NVME_SC_ZNS_INVALID_WRITE;
		goto out;
	}

	if (__check_boundary_error(zns_ftl, slba, nr_lba) == false) {
		// return boundary error
		status = NVME_SC_ZNS_ERR_BOUNDARY;
		goto out;
	}

	// check if slba == current write pointer
	if (slba != zone_descs[zid].wp) {
		NVMEV_ERROR("%s WP error slba 0x%llx nr_lba 0x%llx zone_id %d wp %llx state %d\n",
			    __func__, slba, nr_lba, zid, zns_ftl->zone_descs[zid].wp, state);
		status = NVME_SC_ZNS_INVALID_WRITE;
		goto out;
	}

	switch (state) {
	case ZONE_STATE_EMPTY: {
		// check if slba == start lba in zone
		if (slba != zone_descs[zid].zslba) {
			status = NVME_SC_ZNS_INVALID_WRITE;
			goto out;
		}

		if (is_zone_resource_full(zns_ftl, ACTIVE_ZONE)) {
			status = NVME_SC_ZNS_NO_ACTIVE_ZONE;
			goto out;
		}
		if (is_zone_resource_full(zns_ftl, OPEN_ZONE)) {
			status = NVME_SC_ZNS_NO_OPEN_ZONE;
			goto out;
		}
		acquire_zone_resource(zns_ftl, ACTIVE_ZONE);
		// go through
	}
	case ZONE_STATE_CLOSED: {
		cy_gc = local_clock();
		if (acquire_zone_resource(zns_ftl, OPEN_ZONE) == false) {
			status = NVME_SC_ZNS_NO_OPEN_ZONE;
			goto out;
		}

		// change to ZSIO
		change_zone_state(zns_ftl, zid, ZONE_STATE_OPENED_IMPL);
		cy_gc_e = local_clock();
		cy_IO += (cy_gc_e - cy_gc);
		break;
	}
	case ZONE_STATE_OPENED_IMPL:
	case ZONE_STATE_OPENED_EXPL: {
		break;
	}
	case ZONE_STATE_FULL:
		status = NVME_SC_ZNS_ERR_FULL;
	case ZONE_STATE_READ_ONLY:
		status = NVME_SC_ZNS_ERR_READ_ONLY;
	case ZONE_STATE_OFFLINE:
		status = NVME_SC_ZNS_ERR_OFFLINE;
		goto out;
	}

	__increase_write_ptr(zns_ftl, zid, nr_lba);

	// get delay from nand model
	nsecs_latest = nsecs_start;
	nsecs_latest = ssd_advance_write_buffer(zns_ftl->ssd, nsecs_latest, LBA_TO_BYTE(nr_lba));
	nsecs_xfer_completed = nsecs_latest;

	for (lpn = slpn; lpn <= elpn; lpn += pgs) {
		struct ppa ppa;
		uint64_t pg_off;

		ppa = __lpn_to_ppa(zns_ftl, lpn);
		// [CY]
		ppa.g.cell_mode = action;

		pg_off = ppa.g.pg % spp->pgs_per_oneshotpg;
		pgs = min(elpn - lpn + 1, (uint64_t)(spp->pgs_per_oneshotpg - pg_off));


		/* Aggregate write io in flash page */
		if (((pg_off + pgs) == spp->pgs_per_oneshotpg) || ((lpn + pgs - 1) == zone_elpn)) {
			struct nand_cmd swr = {
				.type = USER_IO,
				.cmd = NAND_WRITE,
				.stime = nsecs_xfer_completed,
				.xfer_size = spp->pgs_per_oneshotpg * spp->pgsz,
				.interleave_pci_dma = false,
				.ppa = &ppa,
			};
			size_t bufs_to_release;
			uint32_t unaligned_space =
				zns_ftl->zp.zone_size % (spp->pgs_per_oneshotpg * spp->pgsz);
			uint64_t nsecs_completed = ssd_advance_nand(zns_ftl->ssd, &swr);

			nsecs_latest = max(nsecs_completed, nsecs_latest);
			NVMEV_ZNS_DEBUG("%s Flush slba 0x%llx nr_lba 0x%lx zone_id %d state %d\n",
					__func__, slba, nr_lba, zid, state);

			if (((lpn + pgs - 1) == zone_elpn) && (unaligned_space > 0))
				bufs_to_release = unaligned_space;
			else
				bufs_to_release = spp->pgs_per_oneshotpg * spp->pgsz;

			schedule_internal_operation(req->sq_id, nsecs_completed, write_buffer,
						    bufs_to_release);
		}
	}

out:
	ret->status = status;
	if ((cmd->control & NVME_RW_FUA) ||
	    (spp->write_early_completion == 0)) /*Wait all flash operations*/
		ret->nsecs_target = nsecs_latest;
	else /*Early completion*/
		ret->nsecs_target = nsecs_xfer_completed;

	return true;
}

static bool __zns_write_zrwa(struct zns_ftl *zns_ftl, struct nvmev_request *req,
			     struct nvmev_result *ret)
{
	struct zone_descriptor *zone_descs = zns_ftl->zone_descs;
	struct ssdparams *spp = &zns_ftl->ssd->sp;
	struct znsparams *zpp = &zns_ftl->zp;
	struct nvme_rw_command *cmd = &(req->cmd->rw);
	uint64_t slba = cmd->slba;
	uint64_t nr_lba = __nr_lbas_from_rw_cmd(cmd);
	uint64_t elba = cmd->slba + nr_lba - 1;

	// get zone from start_lbai
	uint32_t zid = lba_to_zone(zns_ftl, slba);
	enum zone_state state = zone_descs[zid].state;

	uint64_t prev_wp = zone_descs[zid].wp;
	const uint32_t lbas_per_zrwa = zpp->lbas_per_zrwa;
	const uint32_t lbas_per_zrwafg = zpp->lbas_per_zrwafg;
	uint64_t zrwa_impl_start = prev_wp + lbas_per_zrwa;
	uint64_t zrwa_impl_end = prev_wp + (2 * lbas_per_zrwa) - 1;

	uint64_t nsecs_start = req->nsecs_start;
	uint64_t nsecs_completed = nsecs_start;
	uint64_t nsecs_xfer_completed = nsecs_start;
	uint64_t nsecs_latest = nsecs_start;
	uint32_t status = NVME_SC_SUCCESS;

	struct ppa ppa;
	struct nand_cmd swr;

	uint64_t nr_lbas_flush = 0, lpn, remaining, pgs = 0, pg_off;

	NVMEV_DEBUG(
		"%s slba 0x%llx nr_lba 0x%llx zone_id %d state %d wp 0x%llx zrwa_impl_start 0x%llx zrwa_impl_end 0x%llx  buffer %lu\n",
		__func__, slba, nr_lba, zid, state, prev_wp, zrwa_impl_start, zrwa_impl_end,
		zns_ftl->zwra_buffer[zid].remaining);

	if ((LBA_TO_BYTE(nr_lba) % spp->write_unit_size) != 0) {
		status = NVME_SC_ZNS_INVALID_WRITE;
		goto out;
	}

	if (__check_boundary_error(zns_ftl, slba, nr_lba) == false) {
		// return boundary error
		status = NVME_SC_ZNS_ERR_BOUNDARY;
		goto out;
	}

	// valid range : wp <=  <= wp + 2*(size of zwra) -1
	if (slba < zone_descs[zid].wp || elba > zrwa_impl_end) {
		NVMEV_ERROR("%s slba 0x%llx nr_lba 0x%llx zone_id %d wp 0x%llx state %d\n",
			    __func__, slba, nr_lba, zid, zone_descs[zid].wp, state);
		status = NVME_SC_ZNS_INVALID_WRITE;
		goto out;
	}

	switch (state) {
	case ZONE_STATE_CLOSED:
	case ZONE_STATE_EMPTY: {
		if (acquire_zone_resource(zns_ftl, OPEN_ZONE) == false) {
			status = NVME_SC_ZNS_NO_OPEN_ZONE;
			goto out;
		}

		if (!buffer_allocate(&zns_ftl->zwra_buffer[zid], zpp->zrwa_size))
			NVMEV_ASSERT(0);

		// change to ZSIO
		change_zone_state(zns_ftl, zid, ZONE_STATE_OPENED_IMPL);
		break;
	}
	case ZONE_STATE_OPENED_IMPL:
	case ZONE_STATE_OPENED_EXPL: {
		break;
	}
	case ZONE_STATE_FULL:
		status = NVME_SC_ZNS_ERR_FULL;
		goto out;
	case ZONE_STATE_READ_ONLY:
		status = NVME_SC_ZNS_ERR_READ_ONLY;
		goto out;
	case ZONE_STATE_OFFLINE:
		status = NVME_SC_ZNS_ERR_OFFLINE;
		goto out;
#if 0
		case ZONE_STATE_EMPTY :
			return NVME_SC_ZNS_INVALID_ZONE_OPERATION;
#endif
	}

	if (elba >= zrwa_impl_start) {
		nr_lbas_flush = DIV_ROUND_UP((elba - zrwa_impl_start + 1), lbas_per_zrwafg) *
				lbas_per_zrwafg;

		NVMEV_DEBUG("%s implicitly flush zid %d wp before 0x%llx after 0x%llx buffer %lu",
			    __func__, zid, prev_wp, zone_descs[zid].wp + nr_lbas_flush,
			    zns_ftl->zwra_buffer[zid].remaining);
	} else if (elba == zone_to_elba(zns_ftl, zid)) {
		// Workaround. move wp to end of the zone and make state full implicitly
		nr_lbas_flush = elba - prev_wp + 1;

		NVMEV_DEBUG("%s end of zone zid %d wp before 0x%llx after 0x%llx buffer %lu",
			    __func__, zid, prev_wp, zone_descs[zid].wp + nr_lbas_flush,
			    zns_ftl->zwra_buffer[zid].remaining);
	}

	if (nr_lbas_flush > 0) {
		if (!buffer_allocate(&zns_ftl->zwra_buffer[zid], LBA_TO_BYTE(nr_lbas_flush)))
			return false;

		__increase_write_ptr(zns_ftl, zid, nr_lbas_flush);
	}
	// get delay from nand model
	nsecs_latest = nsecs_start;
	nsecs_latest = ssd_advance_write_buffer(zns_ftl->ssd, nsecs_latest, LBA_TO_BYTE(nr_lba));
	nsecs_xfer_completed = nsecs_latest;

	lpn = lba_to_lpn(zns_ftl, prev_wp);
	remaining = nr_lbas_flush / spp->secs_per_pg;
	/* Aggregate write io in flash page */
	while (remaining > 0) {
		ppa = __lpn_to_ppa(zns_ftl, lpn);
		pg_off = ppa.g.pg % spp->pgs_per_oneshotpg;
		pgs = min(remaining, (uint64_t)(spp->pgs_per_oneshotpg - pg_off));

		if ((pg_off + pgs) == spp->pgs_per_oneshotpg) {
			swr.type = USER_IO;
			swr.cmd = NAND_WRITE;
			swr.stime = nsecs_xfer_completed;
			swr.xfer_size = spp->pgs_per_oneshotpg * spp->pgsz;
			swr.interleave_pci_dma = false;
			swr.ppa = &ppa;

			nsecs_completed = ssd_advance_nand(zns_ftl->ssd, &swr);
			nsecs_latest = max(nsecs_completed, nsecs_latest);

			schedule_internal_operation(req->sq_id, nsecs_completed,
						    &zns_ftl->zwra_buffer[zid],
						    spp->pgs_per_oneshotpg * spp->pgsz);
		}

		lpn += pgs;
		remaining -= pgs;
	}

out:
	ret->status = status;

	if ((cmd->control & NVME_RW_FUA) ||
	    (spp->write_early_completion == 0)) /*Wait all flash operations*/
		ret->nsecs_target = nsecs_latest;
	else /*Early completion*/
		ret->nsecs_target = nsecs_xfer_completed;

	return true;
}

bool zns_write(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct zns_ftl *zns_ftl = (struct zns_ftl *)ns->ftls;
	struct zone_descriptor *zone_descs = zns_ftl->zone_descs;
	struct nvme_rw_command *cmd = &(req->cmd->rw);
	uint64_t slpn = lba_to_lpn(zns_ftl, cmd->slba);

	// get zone from start_lba
	uint32_t zid = lpn_to_zone(zns_ftl, slpn);

	NVMEV_DEBUG("%s slba 0x%llx zone_id %d \n", __func__, cmd->slba, zid);

	if (zone_descs[zid].zrwav == 0)
		return __zns_write(zns_ftl, req, ret);
	else
		return __zns_write_zrwa(zns_ftl, req, ret);
}

bool zns_read(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct zns_ftl *zns_ftl = (struct zns_ftl *)ns->ftls;
	struct ssdparams *spp = &zns_ftl->ssd->sp;
	struct zone_descriptor *zone_descs = zns_ftl->zone_descs;
	struct nvme_rw_command *cmd = &(req->cmd->rw);

	uint64_t slba = cmd->slba;
	uint64_t nr_lba = __nr_lbas_from_rw_cmd(cmd);

	uint64_t slpn = lba_to_lpn(zns_ftl, slba);
	uint64_t elpn = lba_to_lpn(zns_ftl, slba + nr_lba - 1);
	uint64_t lpn;

	// get zone from start_lba
	uint32_t zid = lpn_to_zone(zns_ftl, slpn);
	uint32_t status = NVME_SC_SUCCESS;
	uint64_t nsecs_start = req->nsecs_start;
	uint64_t nsecs_completed = nsecs_start, nsecs_latest = 0;
	uint64_t pgs = 0, pg_off;
	struct ppa ppa;
	struct nand_cmd swr;

	NVMEV_ZNS_DEBUG(
		"%s slba 0x%llx nr_lba 0x%lx zone_id %d state %d wp 0x%llx last lba 0x%llx\n",
		__func__, slba, nr_lba, zid, zone_descs[zid].state, zone_descs[zid].wp,
		(slba + nr_lba - 1));

	if (zone_descs[zid].state == ZONE_STATE_OFFLINE) {
		status = NVME_SC_ZNS_ERR_OFFLINE;
	} else if (__check_boundary_error(zns_ftl, slba, nr_lba) == false) {
		// return boundary error
		status = NVME_SC_ZNS_ERR_BOUNDARY;
	}

	// get delay from nand model
	nsecs_latest = nsecs_start;
	if (LBA_TO_BYTE(nr_lba) <= KB(4))
		nsecs_latest += spp->fw_4kb_rd_lat;
	else
		nsecs_latest += spp->fw_rd_lat;

	swr.type = USER_IO;
	swr.cmd = NAND_READ;
	swr.stime = nsecs_latest;
	swr.interleave_pci_dma = false;

	for (lpn = slpn; lpn <= elpn; lpn += pgs) {
		ppa = __lpn_to_ppa(zns_ftl, lpn);
		pg_off = ppa.g.pg % spp->pgs_per_flashpg;
		pgs = min(elpn - lpn + 1, (uint64_t)(spp->pgs_per_flashpg - pg_off));
		swr.xfer_size = pgs * spp->pgsz;
		swr.ppa = &ppa;
		nsecs_completed = ssd_advance_nand(zns_ftl->ssd, &swr);
		nsecs_latest = (nsecs_completed > nsecs_latest) ? nsecs_completed : nsecs_latest;
	}

	if (swr.interleave_pci_dma == false) {
		nsecs_completed = ssd_advance_pcie(zns_ftl->ssd, nsecs_latest, nr_lba * spp->secsz);
		nsecs_latest = (nsecs_completed > nsecs_latest) ? nsecs_completed : nsecs_latest;
	}

	ret->status = status;
	ret->nsecs_target = nsecs_latest;
	return true;
}

// [CY]
/*
	action
	0:LSB 2:CSB 1:MSB, 3:RU
*/

uint32_t get_action(uint32_t stream, uint32_t zid, struct zns_ftl *zns_ftl)
{
	uint32_t write_ratio = zns_ftl->write_traffic[stream];
	uint64_t threshold = zns_ftl->zp.zone_size / 3 * 2;

	if (write_ratio >= threshold) {
		// 1.Check Free Blocks(LSB/CSB/MSB)
		if (zns_ftl->LSB_f < MAX_CELL) return 0;
		else if (zns_ftl->CSB_f < MAX_CELL) return 2;
		else return 1;
	} else {
		// 1.Check Pending Zone Reset(CSB/MSB)
		if (zns_ftl->waiting_reset > 0) return 3;
		// 2.Check Free Blocks(LSB/CSB/MSB)
		if (zns_ftl->LSB_f > zns_ftl->CSB_f) {
			if (zns_ftl->CSB_u > (MAX_CELL/2))
			return (zns_ftl->CSB_u > zns_ftl->MSB_u) ? 1: 2;
			else return 2;
		} else
			return 0;
	}
}
