// SPDX-License-Identifier: GPL-2.0-only

#include "nvmev.h"
#include "ssd.h"
#include "zns_ftl.h"

static uint32_t __zmgmt_send_close_zone(struct zns_ftl *zns_ftl, uint64_t zid)
{
	struct zone_descriptor *zone_descs = zns_ftl->zone_descs;
	enum zone_state cur_state = zone_descs[zid].state;
	uint32_t status = NVME_SC_SUCCESS;

	switch (cur_state) {
	case ZONE_STATE_OPENED_IMPL:
	case ZONE_STATE_OPENED_EXPL:
		change_zone_state(zns_ftl, zid, ZONE_STATE_CLOSED);

		release_zone_resource(zns_ftl, OPEN_ZONE);
		break;

	case ZONE_STATE_CLOSED:
		break;
	default:
		status = NVME_SC_ZNS_INVALID_TRANSITION;
		break;
	}

	return status;
}

static uint32_t __zmgmt_send_finish_zone(struct zns_ftl *zns_ftl, uint64_t zid)
{
	struct zone_descriptor *zone_descs = zns_ftl->zone_descs;
	enum zone_state cur_state = zone_descs[zid].state;
	bool is_zrwa_zone = zone_descs[zid].zrwav;
	uint32_t status = NVME_SC_SUCCESS;

	switch (cur_state) {
	case ZONE_STATE_OPENED_IMPL:
	case ZONE_STATE_OPENED_EXPL:
		release_zone_resource(zns_ftl, OPEN_ZONE);
		// fall through
	case ZONE_STATE_CLOSED:
		release_zone_resource(zns_ftl, ACTIVE_ZONE);

		if (is_zrwa_zone)
			release_zone_resource(zns_ftl, ZRWA_ZONE);

		change_zone_state(zns_ftl, zid, ZONE_STATE_FULL);
		break;

	case ZONE_STATE_EMPTY:
		change_zone_state(zns_ftl, zid, ZONE_STATE_FULL);
		break;
	case ZONE_STATE_FULL:
		break;

	default:
		status = NVME_SC_ZNS_INVALID_TRANSITION;
		break;
	}

	return status;
}

static uint32_t __zmgmt_send_open_zone(struct zns_ftl *zns_ftl, uint64_t zid, uint32_t zrwa)
{
	struct zone_descriptor *zone_descs = zns_ftl->zone_descs;
	enum zone_state cur_state = zone_descs[zid].state;
	uint32_t status = NVME_SC_SUCCESS;

	switch (cur_state) {
	case ZONE_STATE_EMPTY:
		if (is_zone_resource_full(zns_ftl, ACTIVE_ZONE))
			return NVME_SC_ZNS_NO_ACTIVE_ZONE;

		if (is_zone_resource_full(zns_ftl, OPEN_ZONE))
			return NVME_SC_ZNS_NO_OPEN_ZONE;

		if (zrwa) {
			if (is_zone_resource_full(zns_ftl, ZRWA_ZONE))
				return NVME_SC_ZNS_ZRWA_RSRC_UNAVAIL;

			acquire_zone_resource(zns_ftl, ZRWA_ZONE);
			zone_descs[zid].zrwav = 1;
		}

		acquire_zone_resource(zns_ftl, ACTIVE_ZONE);
		// fall through
	case ZONE_STATE_CLOSED:
		if (acquire_zone_resource(zns_ftl, OPEN_ZONE) == false)
			return NVME_SC_ZNS_NO_OPEN_ZONE;
		// fall through
	case ZONE_STATE_OPENED_IMPL:
		change_zone_state(zns_ftl, zid, ZONE_STATE_OPENED_EXPL);
		break;

	case ZONE_STATE_OPENED_EXPL:
		break;
	default:
		status = NVME_SC_ZNS_INVALID_TRANSITION;
		break;
	}

	return status;
}

// [CY]
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


static void __reset_zone(struct zns_ftl *zns_ftl, uint64_t zid, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct zone_descriptor *zone_descs = zns_ftl->zone_descs;
	uint32_t zone_size = zns_ftl->zp.zone_size;
	uint8_t *zone_start_addr = (uint8_t *)get_storage_addr_from_zid(zns_ftl, zid);

	// [CY]
	struct nvme_rw_command *cmd = &(req->cmd->rw);
	uint64_t slba = cmd->slba;
	uint64_t slpn = lba_to_lpn(zns_ftl, slba);
	struct ppa ppa = __lpn_to_ppa(zns_ftl, slpn);
	struct ssdparams *spp = &zns_ftl->ssd->sp;
	uint64_t pg_off = ppa.g.pg % spp->pgs_per_flashpg;

	int i;

/*
	// [CY]
	if (zone_descs[zid].wp == zone_descs[zid].zone_capacity * zid)
		return;
*/
	NVMEV_ZNS_DEBUG("%s ns %d zid %lu start addres 0x%llx zone_size %x \n", __func__,
			zns_ftl->ns, zid, (uint64_t)zone_start_addr, zone_size);


	memset(zone_start_addr, 0, zone_size);

	zone_descs[zid].wp = zone_descs[zid].zslba;
	zone_descs[zid].zrwav = 0;

	if (zns_ftl->zp.zrwa_buffer_size)
		buffer_refill(&zns_ftl->zwra_buffer[zid]);

	// [CY] reset latency
	struct nand_cmd swr = {
		.type = GC_IO,
		.cmd = NAND_ERASE,
		.xfer_size = spp->pgsz, // request size
		.stime = 0,
		.interleave_pci_dma = false,
		.ppa = &ppa,
	};
	ret->nsecs_target = ssd_advance_nand(zns_ftl->ssd, &swr);
	
	// [CY]
	switch (zid%3) {
		case 0: { // LSB
			if (zns_ftl->LSB_r > 0) {
				zns_ftl->LSB_r--;
				zns_ftl->LSB_u--;
				zns_ftl->waiting_reset--;
				//active_lsb--;
			} else { 
				zns_ftl->LSB_r = 0;
				Empty_LSB_reset++;
			}
			zns_ftl->LSB_reset[zns_ftl->LSB_r] = 0;
			if (zns_ftl->LSB_f > 0)
				zns_ftl->LSB_f--;
			break;
		}
		case 2: { // CSB
			if (zns_ftl->CSB_r > 0) {
				zns_ftl->CSB_r--;
				zns_ftl->CSB_u--;
				zns_ftl->waiting_reset--;
				//active_csb--;
			} else { 
				zns_ftl->CSB_r = 0;
				Empty_CSB_reset++;
			}
			zns_ftl->CSB_reset[zns_ftl->CSB_r] = 0;
			if (zns_ftl->CSB_f > 0)
				zns_ftl->CSB_f--;
			break;
		}
		case 1: { // MSB
			if (zns_ftl->MSB_r > 0) {
				zns_ftl->MSB_r--;
				zns_ftl->MSB_u--;
				zns_ftl->waiting_reset--;
				//active_msb--;
			} else { 
				zns_ftl->MSB_r = 0;
				Empty_MSB_reset++;
			}
			zns_ftl->MSB_reset[zns_ftl->MSB_r] = 0;
			if (zns_ftl->MSB_f > 0)
				zns_ftl->MSB_f--;
			break;
		}
	}

	zns_ftl->reset_req_list[zid] = 0;
	zns_ftl->reset_req = 0;

	// [CY]
	total_reset++;

//	printk("active_lsb : %d | active_csb : %d | active_msb : %d\n", active_lsb, active_csb, active_msb);
}

static uint32_t __zmgmt_send_reset_zone(struct zns_ftl *zns_ftl, uint64_t zid, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct zone_descriptor *zone_descs = zns_ftl->zone_descs;
	enum zone_state cur_state = zone_descs[zid].state;
	bool is_zrwa_zone = zone_descs[zid].zrwav;
	uint32_t status = NVME_SC_SUCCESS;

	switch (cur_state) {
	case ZONE_STATE_OPENED_IMPL:
	case ZONE_STATE_OPENED_EXPL:
		release_zone_resource(zns_ftl, OPEN_ZONE);
		// fall through
	case ZONE_STATE_CLOSED:
		release_zone_resource(zns_ftl, ACTIVE_ZONE);

		if (is_zrwa_zone)
			release_zone_resource(zns_ftl, ZRWA_ZONE);
		// fall through
	case ZONE_STATE_FULL:
	case ZONE_STATE_EMPTY:
		change_zone_state(zns_ftl, zid, ZONE_STATE_EMPTY);
		__reset_zone(zns_ftl, zid, req, ret);
		break;

	default:
		status = NVME_SC_ZNS_INVALID_TRANSITION;
		break;
	}

	return status;
}

static uint32_t __zmgmt_send_offline_zone(struct zns_ftl *zns_ftl, uint64_t zid)
{
	enum zone_state cur_state = zns_ftl->zone_descs[zid].state;
	uint32_t status = NVME_SC_SUCCESS;

	switch (cur_state) {
	case ZONE_STATE_READ_ONLY:
		change_zone_state(zns_ftl, zid, ZONE_STATE_OFFLINE);
		break;
	case ZONE_STATE_OFFLINE:
		break;
	default:
		status = NVME_SC_ZNS_INVALID_TRANSITION;
		break;
	}

	return status;
}

static uint32_t __zmgmt_send_flush_explicit_zrwa(struct zns_ftl *zns_ftl, uint64_t slba)
{
	struct zone_descriptor *zone_descs = zns_ftl->zone_descs;
	uint64_t zid = lba_to_zone(zns_ftl, slba);
	uint64_t wp = zone_descs[zid].wp;
	uint32_t status = NVME_SC_SUCCESS;
	enum zone_state cur_state = zone_descs[zid].state;
	uint64_t zone_capacity = zone_descs[zid].zone_capacity;

	const uint32_t lbas_per_zrwafg = zns_ftl->zp.lbas_per_zrwafg;
	const uint32_t lbas_per_zrwa = zns_ftl->zp.lbas_per_zrwa;

	uint64_t zrwa_start = wp;
	uint64_t zrwa_end = min(zrwa_start + lbas_per_zrwa - 1,
				(size_t)zone_to_slba(zns_ftl, zid) + zone_capacity - 1);
	uint64_t nr_lbas_flush = slba - wp + 1;

	NVMEV_ZNS_DEBUG(
		"%s slba 0x%llx zrwa_start 0x%llx zrwa_end 0x%llx zone_descs[zid].zrwav %d\n",
		__func__, slba, zrwa_start, zrwa_end, zone_descs[zid].zrwav);

	if (zone_descs[zid].zrwav == 0)
		return NVME_SC_ZNS_INVALID_ZONE_OPERATION;

	if (!(slba >= zrwa_start && slba <= zrwa_end))
		return NVME_SC_ZNS_INVALID_ZONE_OPERATION;

	if ((nr_lbas_flush % lbas_per_zrwafg) != 0)
		return NVME_SC_INVALID_FIELD;

	switch (cur_state) {
	case ZONE_STATE_OPENED_EXPL:
	case ZONE_STATE_OPENED_IMPL:
	case ZONE_STATE_CLOSED:
		zone_descs[zid].wp = slba + 1;

		if (zone_descs[zid].wp == (zone_to_slba(zns_ftl, zid) + zone_capacity)) {
			//change state to ZSF
			if (cur_state != ZONE_STATE_CLOSED)
				release_zone_resource(zns_ftl, OPEN_ZONE);
			release_zone_resource(zns_ftl, ACTIVE_ZONE);
			release_zone_resource(zns_ftl, ZRWA_ZONE);
			change_zone_state(zns_ftl, zid, ZONE_STATE_FULL);
		}
		break;
	default:
		status = NVME_SC_ZNS_INVALID_ZONE_OPERATION;
		break;
	}

	return status;
}

static uint32_t __zmgmt_send(struct zns_ftl *zns_ftl, uint64_t slba, uint32_t action,
			     uint32_t option, struct nvmev_request *req, struct nvmev_result *ret)
{
	uint32_t status;
	uint64_t zid = lba_to_zone(zns_ftl, slba);

	switch (action) {
	case ZSA_CLOSE_ZONE:
		status = __zmgmt_send_close_zone(zns_ftl, zid);
		break;
	case ZSA_FINISH_ZONE:
		status = __zmgmt_send_finish_zone(zns_ftl, zid);
		break;
	case ZSA_OPEN_ZONE:
		status = __zmgmt_send_open_zone(zns_ftl, zid, option);
		break;
	case ZSA_RESET_ZONE:
		status = __zmgmt_send_reset_zone(zns_ftl, zid, req, ret);
		break;
	case ZSA_OFFLINE_ZONE:
		status = __zmgmt_send_offline_zone(zns_ftl, zid);
		break;
	case ZSA_FLUSH_EXPL_ZRWA:
		status = __zmgmt_send_flush_explicit_zrwa(zns_ftl, slba);
		break;
	}

	return status;
}

void zns_zmgmt_send(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct zns_ftl *zns_ftl = (struct zns_ftl *)ns->ftls;
	struct nvme_zone_mgmt_send *cmd = (struct nvme_zone_mgmt_send *)req->cmd;
	uint32_t select_all = cmd->select_all;
	uint32_t status = NVME_SC_SUCCESS;

	uint32_t action = cmd->zsa;
	uint32_t option = cmd->zsaso;
	uint64_t slba = cmd->slba;
	uint32_t zid = lba_to_zone(zns_ftl, slba);

	uint32_t reset_zid;

	ret->nsecs_target = req->nsecs_start;

	// [CY]
	if (zns_ftl->reset_req != 1) {
		switch (zid%3) {
			case 0: { // LSB zone
				zns_ftl->LSB_reset[zns_ftl->LSB_r] = zid+1;
				zns_ftl->LSB_r++;
				zns_ftl->waiting_reset++;
				zns_ftl->reset_req_list[zid] = 1;
				CY_DEBUG_PRINT("LSB_reset_requests: %d\n", zns_ftl->LSB_r);
				break;
			}
			case 2: { // CSB zone
				zns_ftl->CSB_reset[zns_ftl->CSB_r] = zid+1;
				zns_ftl->CSB_r++;
				zns_ftl->waiting_reset++;
				zns_ftl->reset_req_list[zid] = 1;
				CY_DEBUG_PRINT("CSB_reset_requests: %d\n", zns_ftl->CSB_r);
				break;
			}
			case 1: { // MSB zone
				zns_ftl->MSB_reset[zns_ftl->MSB_r] = zid+1;
				zns_ftl->MSB_r++;
				zns_ftl->waiting_reset++;
				zns_ftl->reset_req_list[zid] = 1;
				CY_DEBUG_PRINT("MSB_reset_requests: %d\n", zns_ftl->MSB_r);
				break;
			}
		}
	}

	// 리셋 요청이 29개 이상 있을 경우 or LSB, CSB, MSB 한 묶음 있을 때
	// reset은 LSB, CSB, MSB 한 묶음으로 진행
	if (zns_ftl->LSB_r > 0 && zns_ftl->CSB_r > 0 && zns_ftl->MSB_r > 0) {
		zns_ftl->reset_req = 1;
	}

	if (zns_ftl->waiting_reset > 29 || zns_ftl->reset_req == 1) {
		CY_DEBUG_PRINT("Run_reset_unit\n");
		// LSB zone reset
		if (zns_ftl->LSB_r > 0) {
			reset_zid = zns_ftl->LSB_r - 1;
			zid = zns_ftl->LSB_reset[reset_zid] - 1;
			if (zid < 0) return;
		}
		else zid = 0;

		__zmgmt_send(zns_ftl, zone_to_slba(zns_ftl, zid), action, option, req, ret);

		// CSB zone reset
		if (zns_ftl->CSB_r > 0) {
			reset_zid = zns_ftl->CSB_r - 1;
			zid = zns_ftl->CSB_reset[reset_zid] - 1;
			if (zid < 0) return; 
		}
		else zid = 2;

		__zmgmt_send(zns_ftl, zone_to_slba(zns_ftl, zid), action, option, req, ret);

		// MSB zone reset
		if (zns_ftl->MSB_r > 0) {
			reset_zid = zns_ftl->MSB_r - 1;
			zid = zns_ftl->MSB_reset[reset_zid] - 1;
			if (zid < 0) return; 
		}
		else zid = 1;

		__zmgmt_send(zns_ftl, zone_to_slba(zns_ftl, zid), action, option, req, ret);
	}
	else if (select_all) {
		printk("select_all\n");
		for (zid = 0; zid < zns_ftl->zp.nr_zones; zid++)
			__zmgmt_send(zns_ftl, zone_to_slba(zns_ftl, zid), action, option, req, ret);
	}


/*	else {
		status = __zmgmt_send(zns_ftl, slba, action, option, req, ret);
	}
*/

	NVMEV_ZNS_DEBUG("%s slba %llx zid %lu select_all %lu action %u status %lu option %lu\n",
			__func__, cmd->slba, zid, select_all, cmd->zsa, status, option);

//	ret->nsecs_target = req->nsecs_start; // no delay
	ret->status = status;

	user_reset++;

	return;
}
