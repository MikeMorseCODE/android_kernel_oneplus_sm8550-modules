// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 * Copyright (c) 2015-2021, The Linux Foundation. All rights reserved.
 */

#define pr_fmt(fmt)	"[drm:%s:%d] " fmt, __func__, __LINE__
#include "sde_encoder_phys.h"
#include "sde_hw_interrupts.h"
#include "sde_core_irq.h"
#include "sde_formats.h"
#include "sde_trace.h"
#if defined(CONFIG_PXLW_IRIS)
#include "dsi_iris_api.h"
#endif
#ifdef OPLUS_FEATURE_DISPLAY
#include "../oplus/oplus_display_panel_common.h"
#include "../oplus/oplus_display_interface.h"
#endif /* OPLUS_FEATURE_DISPLAY */


#ifdef OPLUS_FEATURE_DISPLAY_ADFR
#include "../oplus/oplus_adfr.h"
#endif /* OPLUS_FEATURE_DISPLAY_ADFR */

#ifdef OPLUS_FEATURE_DISPLAY_ONSCREENFINGERPRINT
#include "../oplus/oplus_onscreenfingerprint.h"
#define OPLUS_OFP_ULTRA_LOW_POWER_AOD_VBLANK_TIMEOUT_MS	1000
#endif /* OPLUS_FEATURE_DISPLAY_ONSCREENFINGERPRINT */

#define SDE_DEBUG_CMDENC(e, fmt, ...) SDE_DEBUG("enc%d intf%d " fmt, \
		(e) && (e)->base.parent ? \
		(e)->base.parent->base.id : -1, \
		(e) ? (e)->base.intf_idx - INTF_0 : -1, ##__VA_ARGS__)

#define SDE_ERROR_CMDENC(e, fmt, ...) SDE_ERROR("enc%d intf%d " fmt, \
		(e) && (e)->base.parent ? \
		(e)->base.parent->base.id : -1, \
		(e) ? (e)->base.intf_idx - INTF_0 : -1, ##__VA_ARGS__)

#define to_sde_encoder_phys_cmd(x) \
	container_of(x, struct sde_encoder_phys_cmd, base)

/*
 * Tearcheck sync start and continue thresholds are empirically found
 * based on common panels In the future, may want to allow panels to override
 * these default values
 */
#define DEFAULT_TEARCHECK_SYNC_THRESH_START	4
#define DEFAULT_TEARCHECK_SYNC_THRESH_CONTINUE	4

#define SDE_ENC_WR_PTR_START_TIMEOUT_US 20000
#if defined(PXLW_IRIS_DUAL)
/* decrease polling time, to reduce prepare_commit time */
#define AUTOREFRESH_SEQ1_POLL_TIME	(iris_is_dual_supported() ? 1000 : 2000)
#define AUTOREFRESH_SEQ2_POLL_TIME	(iris_is_dual_supported() ? 1000 : 25000)
#else
#define AUTOREFRESH_SEQ1_POLL_TIME	2000
#define AUTOREFRESH_SEQ2_POLL_TIME	25000
#endif
#define AUTOREFRESH_SEQ2_POLL_TIMEOUT	1000000

static inline int _sde_encoder_phys_cmd_get_idle_timeout(
		struct sde_encoder_phys *phys_enc)
{
	u32 timeout = phys_enc->kickoff_timeout_ms;
	struct sde_encoder_phys_cmd *cmd_enc = to_sde_encoder_phys_cmd(phys_enc);

	return cmd_enc->autorefresh.cfg.frame_count ?
			cmd_enc->autorefresh.cfg.frame_count * timeout : timeout;
}

static inline bool sde_encoder_phys_cmd_is_master(
		struct sde_encoder_phys *phys_enc)
{
	return (phys_enc->split_role != ENC_ROLE_SLAVE) ? true : false;
}

static bool sde_encoder_phys_cmd_mode_fixup(
		struct sde_encoder_phys *phys_enc,
		const struct drm_display_mode *mode,
		struct drm_display_mode *adj_mode)
{
	if (phys_enc)
		SDE_DEBUG_CMDENC(to_sde_encoder_phys_cmd(phys_enc), "\n");
	return true;
}

static uint64_t _sde_encoder_phys_cmd_get_autorefresh_property(
		struct sde_encoder_phys *phys_enc)
{
	struct drm_connector *conn = phys_enc->connector;

	if (!conn || !conn->state)
		return 0;
#if defined(CONFIG_PXLW_IRIS)
	if (iris_is_chip_supported()) {
		if (iris_is_display1_autorefresh_enabled(phys_enc))
			return 1;
	}
#endif

	return sde_connector_get_property(conn->state,
				CONNECTOR_PROP_AUTOREFRESH);
}

static void _sde_encoder_phys_cmd_config_autorefresh(
		struct sde_encoder_phys *phys_enc,
		u32 new_frame_count)
{
	struct sde_encoder_phys_cmd *cmd_enc =
			to_sde_encoder_phys_cmd(phys_enc);
	struct sde_hw_pingpong *hw_pp = phys_enc->hw_pp;
	struct sde_hw_intf *hw_intf = phys_enc->hw_intf;
	struct drm_connector *conn = phys_enc->connector;
	struct sde_hw_autorefresh *cfg_cur, cfg_nxt;

	if (!conn || !conn->state || !hw_pp || !hw_intf)
		return;

	cfg_cur = &cmd_enc->autorefresh.cfg;

	/* autorefresh property value should be validated already */
	memset(&cfg_nxt, 0, sizeof(cfg_nxt));
	cfg_nxt.frame_count = new_frame_count;
	cfg_nxt.enable = (cfg_nxt.frame_count != 0);

	SDE_DEBUG_CMDENC(cmd_enc, "autorefresh state %d->%d framecount %d\n",
			cfg_cur->enable, cfg_nxt.enable, cfg_nxt.frame_count);
	SDE_EVT32(DRMID(phys_enc->parent), hw_pp->idx, hw_intf->idx,
			cfg_cur->enable, cfg_nxt.enable, cfg_nxt.frame_count);

	/* only proceed on state changes */
	if (cfg_nxt.enable == cfg_cur->enable)
		return;

	memcpy(cfg_cur, &cfg_nxt, sizeof(*cfg_cur));

	if (phys_enc->has_intf_te && hw_intf->ops.setup_autorefresh)
		hw_intf->ops.setup_autorefresh(hw_intf, cfg_cur);
	else if (hw_pp->ops.setup_autorefresh)
		hw_pp->ops.setup_autorefresh(hw_pp, cfg_cur);
}

static void _sde_encoder_phys_cmd_update_flush_mask(
		struct sde_encoder_phys *phys_enc)
{
	struct sde_encoder_phys_cmd *cmd_enc;
	struct sde_hw_ctl *ctl;

	if (!phys_enc || !phys_enc->hw_intf || !phys_enc->hw_pp)
		return;

	cmd_enc = to_sde_encoder_phys_cmd(phys_enc);
	ctl = phys_enc->hw_ctl;

	if (!ctl)
		return;

	if (!ctl->ops.update_bitmask) {
		SDE_ERROR("invalid hw_ctl ops %d\n", ctl->idx);
		return;
	}

	ctl->ops.update_bitmask(ctl, SDE_HW_FLUSH_INTF, phys_enc->intf_idx, 1);

	if (phys_enc->hw_pp->merge_3d)
		ctl->ops.update_bitmask(ctl, SDE_HW_FLUSH_MERGE_3D,
			phys_enc->hw_pp->merge_3d->idx, 1);

	SDE_DEBUG_CMDENC(cmd_enc, "update pending flush ctl %d intf_idx %x\n",
			ctl->idx - CTL_0, phys_enc->intf_idx);
}

static void _sde_encoder_phys_cmd_update_intf_cfg(
		struct sde_encoder_phys *phys_enc)
{
	struct sde_encoder_phys_cmd *cmd_enc =
			to_sde_encoder_phys_cmd(phys_enc);
	struct sde_hw_ctl *ctl;

	if (!phys_enc)
		return;

	ctl = phys_enc->hw_ctl;
	if (!ctl)
		return;

	if (ctl->ops.setup_intf_cfg) {
		struct sde_hw_intf_cfg intf_cfg = { 0 };

		intf_cfg.intf = phys_enc->intf_idx;
		intf_cfg.intf_mode_sel = SDE_CTL_MODE_SEL_CMD;
		intf_cfg.stream_sel = cmd_enc->stream_sel;
		intf_cfg.mode_3d =
			sde_encoder_helper_get_3d_blend_mode(phys_enc);
		ctl->ops.setup_intf_cfg(ctl, &intf_cfg);
	} else if (test_bit(SDE_CTL_ACTIVE_CFG, &ctl->caps->features)) {
		sde_encoder_helper_update_intf_cfg(phys_enc);
	}
}

static void sde_encoder_override_tearcheck_rd_ptr(struct sde_encoder_phys *phys_enc)
{
	struct sde_hw_intf *hw_intf;
	struct drm_display_mode *mode;
	struct sde_encoder_phys_cmd *cmd_enc;
	struct sde_hw_pp_vsync_info info[MAX_CHANNELS_PER_ENC] = {{0}};
	u32 adjusted_tear_rd_ptr_line_cnt;

	if (!phys_enc || !phys_enc->hw_intf)
		return;

	cmd_enc = to_sde_encoder_phys_cmd(phys_enc);
	hw_intf = phys_enc->hw_intf;
	mode = &phys_enc->cached_mode;

	/* Configure TE rd_ptr_val to the end of qsync Start Window.
	 * This ensures next frame trigger_start does not get latched in the current
	 * vsync window.
	 */
	adjusted_tear_rd_ptr_line_cnt = mode->vdisplay + cmd_enc->qsync_threshold_lines + 1;

	if (hw_intf && hw_intf->ops.override_tear_rd_ptr_val)
		hw_intf->ops.override_tear_rd_ptr_val(hw_intf, adjusted_tear_rd_ptr_line_cnt);

	sde_encoder_helper_get_pp_line_count(phys_enc->parent, info);
	SDE_EVT32_VERBOSE(phys_enc->hw_intf->idx - INTF_0, mode->vdisplay,
		cmd_enc->qsync_threshold_lines, adjusted_tear_rd_ptr_line_cnt,
		info[0].rd_ptr_line_count, info[0].rd_ptr_frame_count, info[0].wr_ptr_line_count,
		info[1].rd_ptr_line_count, info[1].rd_ptr_frame_count, info[1].wr_ptr_line_count);
}

static void _sde_encoder_phys_signal_frame_done(struct sde_encoder_phys *phys_enc)
{
	struct sde_encoder_phys_cmd *cmd_enc;
	struct sde_hw_ctl *ctl;
	u32 scheduler_status = INVALID_CTL_STATUS, event = 0;
	struct sde_hw_pp_vsync_info info[MAX_CHANNELS_PER_ENC] = {{0}};

	cmd_enc = to_sde_encoder_phys_cmd(phys_enc);
	ctl = phys_enc->hw_ctl;

	if (!ctl)
		return;

	/* notify all synchronous clients first, then asynchronous clients */
	if (phys_enc->parent_ops.handle_frame_done &&
		atomic_add_unless(&phys_enc->pending_kickoff_cnt, -1, 0)) {
		event = SDE_ENCODER_FRAME_EVENT_DONE |
			SDE_ENCODER_FRAME_EVENT_SIGNAL_RELEASE_FENCE;
		spin_lock(phys_enc->enc_spinlock);
		phys_enc->parent_ops.handle_frame_done(phys_enc->parent,
				phys_enc, event);
		if (cmd_enc->frame_tx_timeout_report_cnt)
			phys_enc->recovered = true;
		spin_unlock(phys_enc->enc_spinlock);
	}

	if (ctl->ops.get_scheduler_status)
		scheduler_status = ctl->ops.get_scheduler_status(ctl);

	sde_encoder_helper_get_pp_line_count(phys_enc->parent, info);
	SDE_EVT32_IRQ(DRMID(phys_enc->parent), ctl->idx - CTL_0, phys_enc->hw_pp->idx - PINGPONG_0,
		event, scheduler_status, phys_enc->autorefresh_disable_trans, info[0].pp_idx,
		info[0].intf_idx, info[0].intf_frame_count, info[0].wr_ptr_line_count,
		info[0].rd_ptr_line_count, info[1].pp_idx, info[1].intf_idx,
		info[1].intf_frame_count, info[1].wr_ptr_line_count, info[1].rd_ptr_line_count);

	/*
	 * For hw-fences, in the last frame during the autorefresh disable transition
	 * hw won't trigger the output-fence signal once the frame is done, therefore
	 * sw must trigger the override to force the signal here
	 */
	if (phys_enc->autorefresh_disable_trans) {
		if (ctl->ops.trigger_output_fence_override)
			ctl->ops.trigger_output_fence_override(ctl);
		phys_enc->autorefresh_disable_trans = false;
	}

	/* Signal any waiting atomic commit thread */
	wake_up_all(&phys_enc->pending_kickoff_wq);

#ifdef OPLUS_FEATURE_DISPLAY_ADFR
	oplus_adfr_irq_handler(phys_enc, OPLUS_ADFR_PP_DONE);
#endif /* OPLUS_FEATURE_DISPLAY_ADFR */

#ifdef OPLUS_FEATURE_DISPLAY_ONSCREENFINGERPRINT
	if (oplus_ofp_is_supported()) {
		oplus_ofp_pressed_icon_status_update(phys_enc, OPLUS_OFP_PP_DONE);
		oplus_ofp_aod_display_on_set(phys_enc);
	}
#endif /* OPLUS_FEATURE_DISPLAY_ONSCREENFINGERPRINT */
}

static void sde_encoder_phys_cmd_ctl_done_irq(void *arg, int irq_idx)
{
	struct sde_encoder_phys *phys_enc = arg;

	if (!phys_enc)
		return;

	SDE_ATRACE_BEGIN("ctl_done_irq");

	_sde_encoder_phys_signal_frame_done(phys_enc);

	SDE_ATRACE_END("ctl_done_irq");
}

static void sde_encoder_phys_cmd_pp_tx_done_irq(void *arg, int irq_idx)
{
	struct sde_encoder_phys *phys_enc = arg;

	if (!phys_enc || !phys_enc->hw_pp)
		return;

	SDE_ATRACE_BEGIN("pp_done_irq");

	_sde_encoder_phys_signal_frame_done(phys_enc);

	SDE_ATRACE_END("pp_done_irq");
}

static void sde_encoder_phys_cmd_autorefresh_done_irq(void *arg, int irq_idx)
{
	struct sde_encoder_phys *phys_enc = arg;
	struct sde_encoder_phys_cmd *cmd_enc =
			to_sde_encoder_phys_cmd(phys_enc);
	unsigned long lock_flags;
	int new_cnt;

	if (!cmd_enc)
		return;

	phys_enc = &cmd_enc->base;
	spin_lock_irqsave(phys_enc->enc_spinlock, lock_flags);
	new_cnt = atomic_add_unless(&cmd_enc->autorefresh.kickoff_cnt, -1, 0);
	spin_unlock_irqrestore(phys_enc->enc_spinlock, lock_flags);

	SDE_EVT32_IRQ(DRMID(phys_enc->parent), phys_enc->hw_pp->idx - PINGPONG_0,
			phys_enc->hw_intf->idx - INTF_0, new_cnt);

	if (new_cnt)
		_sde_encoder_phys_signal_frame_done(phys_enc);

	/* Signal any waiting atomic commit thread */
	wake_up_all(&cmd_enc->autorefresh.kickoff_wq);
}

static void sde_encoder_phys_cmd_te_rd_ptr_irq(void *arg, int irq_idx)
{
	struct sde_encoder_phys *phys_enc = arg;
	struct sde_encoder_phys_cmd *cmd_enc;
	u32 scheduler_status = INVALID_CTL_STATUS;
	struct sde_hw_ctl *ctl;
	struct sde_hw_pp_vsync_info info[MAX_CHANNELS_PER_ENC] = {{0}};
	struct sde_encoder_phys_cmd_te_timestamp *te_timestamp;
	unsigned long lock_flags;
	u32 fence_ready = 0;
#if defined(CONFIG_PXLW_IRIS) || defined(CONFIG_PXLW_SOFT_IRIS) || defined(OPLUS_FEATURE_DISPLAY)
	struct sde_connector *conn;
#endif

	if (!phys_enc || !phys_enc->hw_pp || !phys_enc->hw_intf || !phys_enc->hw_ctl)
		return;

#if defined(CONFIG_PXLW_IRIS) || defined(CONFIG_PXLW_SOFT_IRIS)
	if (iris_is_chip_supported() || iris_is_softiris_supported()) {
		conn = to_sde_connector(phys_enc->connector);
		if (conn) {
			spin_lock_irqsave(&conn->bl_spinlock, lock_flags);
			conn->rd_ptr_ktime = ktime_get();
			spin_unlock_irqrestore(&conn->bl_spinlock, lock_flags);
		}
	}
#endif

	SDE_ATRACE_BEGIN("rd_ptr_irq");
	cmd_enc = to_sde_encoder_phys_cmd(phys_enc);
	ctl = phys_enc->hw_ctl;

	if (ctl->ops.get_scheduler_status)
		scheduler_status = ctl->ops.get_scheduler_status(ctl);

	spin_lock_irqsave(phys_enc->enc_spinlock, lock_flags);
	te_timestamp = list_first_entry_or_null(&cmd_enc->te_timestamp_list,
				struct sde_encoder_phys_cmd_te_timestamp, list);
	if (te_timestamp) {
		list_del_init(&te_timestamp->list);
		te_timestamp->timestamp = ktime_get();
		list_add_tail(&te_timestamp->list, &cmd_enc->te_timestamp_list);
	}
	spin_unlock_irqrestore(phys_enc->enc_spinlock, lock_flags);

#ifdef OPLUS_FEATURE_DISPLAY
        conn = to_sde_connector(phys_enc->connector);
        if (conn && te_timestamp) {
            oplus_save_te_timestamp(conn, te_timestamp->timestamp);
        }
#endif /* OPLUS_FEATURE_DISPLAY */

	if ((scheduler_status != 0x1) && ctl->ops.get_hw_fence_status)
		fence_ready = ctl->ops.get_hw_fence_status(ctl);

	sde_encoder_helper_get_pp_line_count(phys_enc->parent, info);
	SDE_EVT32_IRQ(DRMID(phys_enc->parent), scheduler_status, fence_ready, info[0].pp_idx,
		info[0].intf_idx, info[0].intf_frame_count, info[0].wr_ptr_line_count,
		info[0].rd_ptr_line_count, info[1].pp_idx, info[1].intf_idx,
		info[1].intf_frame_count, info[1].wr_ptr_line_count, info[1].rd_ptr_line_count);

	if (phys_enc->parent_ops.handle_vblank_virt)
		phys_enc->parent_ops.handle_vblank_virt(phys_enc->parent,
			phys_enc);

	atomic_add_unless(&cmd_enc->pending_vblank_cnt, -1, 0);
	wake_up_all(&cmd_enc->pending_vblank_wq);

#ifdef OPLUS_FEATURE_DISPLAY
	conn = to_sde_connector(phys_enc->connector);
	if (conn) {
		oplus_panel_cmdq_pack_status_reset(conn);
		oplus_set_pwm_switch_cmd_te_flag(conn);
	}
#endif /* OPLUS_FEATURE_DISPLAY */

#ifdef OPLUS_FEATURE_DISPLAY_ADFR
	oplus_adfr_irq_handler(phys_enc, OPLUS_ADFR_RD_PTR);
#endif /* OPLUS_FEATURE_DISPLAY_ADFR */

#ifdef OPLUS_FEATURE_DISPLAY_ONSCREENFINGERPRINT
	if (oplus_ofp_is_supported()) {
		oplus_ofp_aod_off_hbm_on_delay_check(phys_enc);
		oplus_ofp_pressed_icon_status_update(phys_enc, OPLUS_OFP_RD_PTR);
		oplus_ofp_panel_hbm_status_update(phys_enc);
		oplus_ofp_notify_uiready(phys_enc);
	}
#endif /* OPLUS_FEATURE_DISPLAY_ONSCREENFINGERPRINT */

	SDE_ATRACE_END("rd_ptr_irq");
}

static void sde_encoder_phys_cmd_wr_ptr_irq(void *arg, int irq_idx)
{
	struct sde_encoder_phys *phys_enc = arg;
	struct sde_hw_ctl *ctl;
	u32 event = 0, qsync_mode = 0;
	struct sde_hw_pp_vsync_info info[MAX_CHANNELS_PER_ENC] = {{0}};

	if (!phys_enc || !phys_enc->hw_ctl)
		return;

	SDE_ATRACE_BEGIN("wr_ptr_irq");
	ctl = phys_enc->hw_ctl;
	qsync_mode = sde_connector_get_qsync_mode(phys_enc->connector);

	if (atomic_add_unless(&phys_enc->pending_retire_fence_cnt, -1, 0)) {
		event = SDE_ENCODER_FRAME_EVENT_SIGNAL_RETIRE_FENCE;
		if (phys_enc->parent_ops.handle_frame_done) {
			spin_lock(phys_enc->enc_spinlock);
			phys_enc->parent_ops.handle_frame_done(
					phys_enc->parent, phys_enc, event);
			spin_unlock(phys_enc->enc_spinlock);
		}
	}

	sde_encoder_helper_get_pp_line_count(phys_enc->parent, info);
	SDE_EVT32_IRQ(DRMID(phys_enc->parent), ctl->idx - CTL_0, event, qsync_mode,
		info[0].pp_idx, info[0].intf_idx, info[0].intf_frame_count,
		info[0].wr_ptr_line_count, info[0].rd_ptr_line_count, info[1].pp_idx,
		info[1].intf_idx, info[1].intf_frame_count, info[1].wr_ptr_line_count,
		info[1].rd_ptr_line_count);

	if (qsync_mode)
		sde_encoder_override_tearcheck_rd_ptr(phys_enc);

	/* Signal any waiting wr_ptr start interrupt */
	wake_up_all(&phys_enc->pending_kickoff_wq);

#ifdef OPLUS_FEATURE_DISPLAY_ADFR
	oplus_adfr_irq_handler(phys_enc, OPLUS_ADFR_WD_PTR);
#endif /* OPLUS_FEATURE_DISPLAY_ADFR */

#ifdef OPLUS_FEATURE_DISPLAY_ONSCREENFINGERPRINT
	if (oplus_ofp_is_supported()) {
		oplus_ofp_pressed_icon_status_update(phys_enc, OPLUS_OFP_WD_PTR);
	}
#endif /* OPLUS_FEATURE_DISPLAY_ONSCREENFINGERPRINT */

	SDE_ATRACE_END("wr_ptr_irq");
}

static void _sde_encoder_phys_cmd_setup_irq_hw_idx(
		struct sde_encoder_phys *phys_enc)
{
	struct sde_encoder_irq *irq;
	struct sde_kms *sde_kms;

	if (!phys_enc->sde_kms || !phys_enc->hw_pp || !phys_enc->hw_ctl) {
		SDE_ERROR("invalid args %d %d %d\n", !phys_enc->sde_kms,
				!phys_enc->hw_pp, !phys_enc->hw_ctl);
		return;
	}

	if (phys_enc->has_intf_te && !phys_enc->hw_intf) {
		SDE_ERROR("invalid intf configuration\n");
		return;
	}

	sde_kms = phys_enc->sde_kms;

	irq = &phys_enc->irq[INTR_IDX_CTL_START];
	irq->hw_idx = phys_enc->hw_ctl->idx;

	irq = &phys_enc->irq[INTR_IDX_CTL_DONE];
	irq->hw_idx = phys_enc->hw_ctl->idx;

	irq = &phys_enc->irq[INTR_IDX_PINGPONG];
	irq->hw_idx = phys_enc->hw_pp->idx;

	irq = &phys_enc->irq[INTR_IDX_RDPTR];
	if (phys_enc->has_intf_te)
		irq->hw_idx = phys_enc->hw_intf->idx;
	else
		irq->hw_idx = phys_enc->hw_pp->idx;

	irq = &phys_enc->irq[INTR_IDX_AUTOREFRESH_DONE];
	if (phys_enc->has_intf_te)
		irq->hw_idx = phys_enc->hw_intf->idx;
	else
		irq->hw_idx = phys_enc->hw_pp->idx;

	irq = &phys_enc->irq[INTR_IDX_WRPTR];
	if (phys_enc->has_intf_te)
		irq->hw_idx = phys_enc->hw_intf->idx;
	else
		irq->hw_idx = phys_enc->hw_pp->idx;
}

static void sde_encoder_phys_cmd_cont_splash_mode_set(
		struct sde_encoder_phys *phys_enc,
		struct drm_display_mode *adj_mode)
{
	struct sde_hw_intf *hw_intf;
	struct sde_hw_pingpong *hw_pp;
	struct sde_encoder_phys_cmd *cmd_enc;

	if (!phys_enc || !adj_mode) {
		SDE_ERROR("invalid args\n");
		return;
	}

	phys_enc->cached_mode = *adj_mode;
	phys_enc->enable_state = SDE_ENC_ENABLED;

	if (!phys_enc->hw_ctl || !phys_enc->hw_pp) {
		SDE_DEBUG("invalid ctl:%d pp:%d\n",
			(phys_enc->hw_ctl == NULL),
			(phys_enc->hw_pp == NULL));
		return;
	}

	if (sde_encoder_phys_cmd_is_master(phys_enc)) {
		cmd_enc = to_sde_encoder_phys_cmd(phys_enc);
		hw_pp = phys_enc->hw_pp;
		hw_intf = phys_enc->hw_intf;

		if (phys_enc->has_intf_te && hw_intf &&
				hw_intf->ops.get_autorefresh) {
			hw_intf->ops.get_autorefresh(hw_intf,
					&cmd_enc->autorefresh.cfg);
		} else if (hw_pp && hw_pp->ops.get_autorefresh) {
			hw_pp->ops.get_autorefresh(hw_pp,
					&cmd_enc->autorefresh.cfg);
		}

		if (hw_intf && hw_intf->ops.reset_counter)
			hw_intf->ops.reset_counter(hw_intf);
	}

	_sde_encoder_phys_cmd_setup_irq_hw_idx(phys_enc);
}

static void sde_encoder_phys_cmd_mode_set(
		struct sde_encoder_phys *phys_enc,
		struct drm_display_mode *mode,
		struct drm_display_mode *adj_mode, bool *reinit_mixers)
{
	struct sde_encoder_phys_cmd *cmd_enc =
		to_sde_encoder_phys_cmd(phys_enc);
	struct sde_rm *rm = &phys_enc->sde_kms->rm;
	struct sde_rm_hw_iter iter;
	int i, instance;

	if (!phys_enc || !mode || !adj_mode) {
		SDE_ERROR("invalid args\n");
		return;
	}
	phys_enc->cached_mode = *adj_mode;
	SDE_DEBUG_CMDENC(cmd_enc, "caching mode:\n");
	drm_mode_debug_printmodeline(adj_mode);

	instance = phys_enc->split_role == ENC_ROLE_SLAVE ? 1 : 0;

	/* Retrieve previously allocated HW Resources. Shouldn't fail */
	sde_rm_init_hw_iter(&iter, phys_enc->parent->base.id, SDE_HW_BLK_CTL);
	for (i = 0; i <= instance; i++) {
		if (sde_rm_get_hw(rm, &iter)) {
			if (phys_enc->hw_ctl && phys_enc->hw_ctl != to_sde_hw_ctl(iter.hw)) {
				*reinit_mixers =  true;
				SDE_EVT32(phys_enc->hw_ctl->idx,
						to_sde_hw_ctl(iter.hw)->idx);
			}
			phys_enc->hw_ctl = to_sde_hw_ctl(iter.hw);
		}
	}

	if (IS_ERR_OR_NULL(phys_enc->hw_ctl)) {
		SDE_ERROR_CMDENC(cmd_enc, "failed to init ctl: %ld\n",
				PTR_ERR(phys_enc->hw_ctl));
		phys_enc->hw_ctl = NULL;
		return;
	}

	sde_rm_init_hw_iter(&iter, phys_enc->parent->base.id, SDE_HW_BLK_INTF);
	for (i = 0; i <= instance; i++) {
		if (sde_rm_get_hw(rm, &iter))
			phys_enc->hw_intf = to_sde_hw_intf(iter.hw);
	}

	if (IS_ERR_OR_NULL(phys_enc->hw_intf)) {
		SDE_ERROR_CMDENC(cmd_enc, "failed to init intf: %ld\n",
				PTR_ERR(phys_enc->hw_intf));
		phys_enc->hw_intf = NULL;
		return;
	}

	_sde_encoder_phys_cmd_setup_irq_hw_idx(phys_enc);

	phys_enc->kickoff_timeout_ms =
		sde_encoder_helper_get_kickoff_timeout_ms(phys_enc->parent);
}

static int _sde_encoder_phys_cmd_handle_framedone_timeout(
		struct sde_encoder_phys *phys_enc)
{
	struct sde_encoder_phys_cmd *cmd_enc =
			to_sde_encoder_phys_cmd(phys_enc);
	bool recovery_events = sde_encoder_recovery_events_enabled(
			phys_enc->parent);
	u32 frame_event = SDE_ENCODER_FRAME_EVENT_ERROR
				| SDE_ENCODER_FRAME_EVENT_SIGNAL_RELEASE_FENCE;
	struct drm_connector *conn;
	u32 pending_kickoff_cnt;
	unsigned long lock_flags;

	if (!phys_enc->hw_pp || !phys_enc->hw_ctl)
		return -EINVAL;

	conn = phys_enc->connector;

	/* decrement the kickoff_cnt before checking for ESD status */
	if (!atomic_add_unless(&phys_enc->pending_kickoff_cnt, -1, 0))
		return 0;

	cmd_enc->frame_tx_timeout_report_cnt++;
	pending_kickoff_cnt = atomic_read(&phys_enc->pending_kickoff_cnt) + 1;

	SDE_EVT32(DRMID(phys_enc->parent), phys_enc->hw_pp->idx - PINGPONG_0,
			cmd_enc->frame_tx_timeout_report_cnt,
			pending_kickoff_cnt,
			frame_event);

	/* check if panel is still sending TE signal or not */
	if (sde_connector_esd_status(phys_enc->connector))
		goto exit;

	/* to avoid flooding, only log first time, and "dead" time */
	if (cmd_enc->frame_tx_timeout_report_cnt == 1) {
		SDE_ERROR_CMDENC(cmd_enc,
				"pp:%d kickoff timed out ctl %d koff_cnt %d\n",
				phys_enc->hw_pp->idx - PINGPONG_0,
				phys_enc->hw_ctl->idx - CTL_0,
				pending_kickoff_cnt);

		SDE_EVT32(DRMID(phys_enc->parent), SDE_EVTLOG_FATAL);
		mutex_lock(phys_enc->vblank_ctl_lock);
		sde_encoder_helper_unregister_irq(phys_enc, INTR_IDX_RDPTR);
		if (sde_kms_is_secure_session_inprogress(phys_enc->sde_kms))
			SDE_DBG_DUMP(SDE_DBG_BUILT_IN_ALL, "secure");
		else
			SDE_DBG_DUMP(SDE_DBG_BUILT_IN_ALL);
		sde_encoder_helper_register_irq(phys_enc, INTR_IDX_RDPTR);
		mutex_unlock(phys_enc->vblank_ctl_lock);
	}

	/*
	 * if the recovery event is registered by user, don't panic
	 * trigger panic on first timeout if no listener registered
	 */
	if (recovery_events)
		sde_connector_event_notify(conn, DRM_EVENT_SDE_HW_RECOVERY,
				sizeof(uint8_t), SDE_RECOVERY_CAPTURE);
	else if (cmd_enc->frame_tx_timeout_report_cnt)
		SDE_DBG_DUMP(0x0, "panic");

	/* request a ctl reset before the next kickoff */
	phys_enc->enable_state = SDE_ENC_ERR_NEEDS_HW_RESET;

exit:
	if (phys_enc->parent_ops.handle_frame_done) {
		spin_lock_irqsave(phys_enc->enc_spinlock, lock_flags);
		phys_enc->parent_ops.handle_frame_done(
				phys_enc->parent, phys_enc, frame_event);
		spin_unlock_irqrestore(phys_enc->enc_spinlock, lock_flags);
	}

	return -ETIMEDOUT;
}

static bool _sde_encoder_phys_is_ppsplit_slave(
		struct sde_encoder_phys *phys_enc)
{
	if (!phys_enc)
		return false;

	return _sde_encoder_phys_is_ppsplit(phys_enc) &&
			phys_enc->split_role == ENC_ROLE_SLAVE;
}

static bool _sde_encoder_phys_is_disabling_ppsplit_slave(
		struct sde_encoder_phys *phys_enc)
{
	enum sde_rm_topology_name old_top;

	if (!phys_enc || !phys_enc->connector ||
			phys_enc->split_role != ENC_ROLE_SLAVE)
		return false;

	old_top = sde_connector_get_old_topology_name(
			phys_enc->connector->state);

	return old_top == SDE_RM_TOPOLOGY_PPSPLIT;
}

static int _sde_encoder_phys_cmd_poll_write_pointer_started(
		struct sde_encoder_phys *phys_enc)
{
	struct sde_encoder_phys_cmd *cmd_enc =
			to_sde_encoder_phys_cmd(phys_enc);
	struct sde_hw_pingpong *hw_pp = phys_enc->hw_pp;
	struct sde_hw_intf *hw_intf = phys_enc->hw_intf;
	struct sde_hw_pp_vsync_info info;
	u32 timeout_us = SDE_ENC_WR_PTR_START_TIMEOUT_US;
	int ret = 0;

	if (!hw_pp || !hw_intf)
		return 0;

	if (phys_enc->has_intf_te) {
		if (!hw_intf->ops.get_vsync_info ||
				!hw_intf->ops.poll_timeout_wr_ptr)
			goto end;
	} else {
		if (!hw_pp->ops.get_vsync_info ||
				!hw_pp->ops.poll_timeout_wr_ptr)
			goto end;
	}

	if (phys_enc->has_intf_te)
		ret = hw_intf->ops.get_vsync_info(hw_intf, &info);
	else
		ret = hw_pp->ops.get_vsync_info(hw_pp, &info);

	if (ret)
		return ret;

	SDE_DEBUG_CMDENC(cmd_enc,
			"pp:%d intf:%d rd_ptr %d wr_ptr %d\n",
			phys_enc->hw_pp->idx - PINGPONG_0,
			phys_enc->hw_intf->idx - INTF_0,
			info.rd_ptr_line_count,
			info.wr_ptr_line_count);
	SDE_EVT32_VERBOSE(DRMID(phys_enc->parent),
			phys_enc->hw_pp->idx - PINGPONG_0,
			phys_enc->hw_intf->idx - INTF_0,
			info.wr_ptr_line_count);

	if (phys_enc->has_intf_te)
		ret = hw_intf->ops.poll_timeout_wr_ptr(hw_intf, timeout_us);
	else
		ret = hw_pp->ops.poll_timeout_wr_ptr(hw_pp, timeout_us);

	if (ret) {
		SDE_EVT32(DRMID(phys_enc->parent), phys_enc->hw_pp->idx - PINGPONG_0,
				phys_enc->hw_intf->idx - INTF_0, timeout_us, ret);
		SDE_DBG_DUMP(SDE_DBG_BUILT_IN_ALL, "panic");
	}

end:
	return ret;
}

static bool _sde_encoder_phys_cmd_is_ongoing_pptx(
		struct sde_encoder_phys *phys_enc)
{
	struct sde_hw_pingpong *hw_pp;
	struct sde_hw_pp_vsync_info info;
	struct sde_hw_intf *hw_intf;

	if (!phys_enc)
		return false;

	if (phys_enc->has_intf_te) {
		hw_intf = phys_enc->hw_intf;
		if (!hw_intf || !hw_intf->ops.get_vsync_info)
			return false;

		hw_intf->ops.get_vsync_info(hw_intf, &info);
	} else {
		hw_pp = phys_enc->hw_pp;
		if (!hw_pp || !hw_pp->ops.get_vsync_info)
			return false;

		hw_pp->ops.get_vsync_info(hw_pp, &info);
	}

	SDE_EVT32(DRMID(phys_enc->parent), phys_enc->hw_pp->idx - PINGPONG_0,
		phys_enc->hw_intf->idx - INTF_0, atomic_read(&phys_enc->pending_kickoff_cnt),
		info.wr_ptr_line_count, info.intf_frame_count, phys_enc->cached_mode.vdisplay);

	if (info.wr_ptr_line_count > 0 && info.wr_ptr_line_count <
			phys_enc->cached_mode.vdisplay)
		return true;

	return false;
}

static bool _sde_encoder_phys_cmd_is_scheduler_idle(
		struct sde_encoder_phys *phys_enc)
{
	bool wr_ptr_wait_success = true;
	unsigned long lock_flags;
	bool ret = false;
	struct sde_encoder_phys_cmd *cmd_enc =
			to_sde_encoder_phys_cmd(phys_enc);
	struct sde_hw_ctl *ctl = phys_enc->hw_ctl;
	enum frame_trigger_mode_type frame_trigger_mode =
			phys_enc->frame_trigger_mode;

	if (sde_encoder_phys_cmd_is_master(phys_enc))
		wr_ptr_wait_success = cmd_enc->wr_ptr_wait_success;

	/*
	 * Handle cases where a pp-done interrupt is missed
	 * due to irq latency with POSTED start
	 */
	if (wr_ptr_wait_success &&
		(frame_trigger_mode == FRAME_DONE_WAIT_POSTED_START) &&
		ctl->ops.get_scheduler_status &&
		phys_enc->parent_ops.handle_frame_done &&
		atomic_read(&phys_enc->pending_kickoff_cnt) > 0 &&
		(ctl->ops.get_scheduler_status(ctl) & BIT(0)) &&
		atomic_add_unless(&phys_enc->pending_kickoff_cnt, -1, 0)) {

		spin_lock_irqsave(phys_enc->enc_spinlock, lock_flags);
		phys_enc->parent_ops.handle_frame_done(
			phys_enc->parent, phys_enc,
			SDE_ENCODER_FRAME_EVENT_DONE |
			SDE_ENCODER_FRAME_EVENT_SIGNAL_RELEASE_FENCE);
		spin_unlock_irqrestore(phys_enc->enc_spinlock, lock_flags);

		SDE_EVT32(DRMID(phys_enc->parent),
			phys_enc->hw_pp->idx - PINGPONG_0,
			phys_enc->hw_intf->idx - INTF_0,
			atomic_read(&phys_enc->pending_kickoff_cnt));

		ret = true;
	}

	return ret;
}

static int _sde_encoder_phys_cmd_wait_for_idle(
		struct sde_encoder_phys *phys_enc)
{
	struct sde_encoder_wait_info wait_info = {0};
	enum sde_intr_idx intr_idx;
	int ret;

	if (!phys_enc) {
		SDE_ERROR("invalid encoder\n");
		return -EINVAL;
	}

	if (sde_encoder_check_ctl_done_support(phys_enc->parent)
			&& !sde_encoder_phys_cmd_is_master(phys_enc))
		return 0;

	if (atomic_read(&phys_enc->pending_kickoff_cnt) > 1)
		wait_info.count_check = 1;

	wait_info.wq = &phys_enc->pending_kickoff_wq;
	wait_info.atomic_cnt = &phys_enc->pending_kickoff_cnt;
	wait_info.timeout_ms = phys_enc->kickoff_timeout_ms;

	/* slave encoder doesn't enable for ppsplit */
	if (_sde_encoder_phys_is_ppsplit_slave(phys_enc))
		return 0;

	if (_sde_encoder_phys_cmd_is_scheduler_idle(phys_enc))
		return 0;

	intr_idx = sde_encoder_check_ctl_done_support(phys_enc->parent) ?
				INTR_IDX_CTL_DONE : INTR_IDX_PINGPONG;

	ret = sde_encoder_helper_wait_for_irq(phys_enc, intr_idx, &wait_info);
	if (ret == -ETIMEDOUT) {
		if (_sde_encoder_phys_cmd_is_scheduler_idle(phys_enc))
			return 0;
		_sde_encoder_phys_cmd_handle_framedone_timeout(phys_enc);
	}

	return ret;
}

static int _sde_encoder_phys_cmd_wait_for_autorefresh_done(
		struct sde_encoder_phys *phys_enc)
{
	struct sde_encoder_phys_cmd *cmd_enc =
			to_sde_encoder_phys_cmd(phys_enc);
	struct sde_encoder_wait_info wait_info = {0};
	int ret = 0;

	if (!phys_enc) {
		SDE_ERROR("invalid encoder\n");
		return -EINVAL;
	}

	/* only master deals with autorefresh */
	if (!sde_encoder_phys_cmd_is_master(phys_enc))
		return 0;

	wait_info.wq = &cmd_enc->autorefresh.kickoff_wq;
	wait_info.atomic_cnt = &cmd_enc->autorefresh.kickoff_cnt;
	wait_info.timeout_ms = _sde_encoder_phys_cmd_get_idle_timeout(phys_enc);

	/* wait for autorefresh kickoff to start */
	ret = sde_encoder_helper_wait_for_irq(phys_enc,
			INTR_IDX_AUTOREFRESH_DONE, &wait_info);

	/* double check that kickoff has started by reading write ptr reg */
	if (!ret)
		ret = _sde_encoder_phys_cmd_poll_write_pointer_started(
			phys_enc);
	else
		sde_encoder_helper_report_irq_timeout(phys_enc,
				INTR_IDX_AUTOREFRESH_DONE);

	return ret;
}

static int sde_encoder_phys_cmd_control_vblank_irq(
		struct sde_encoder_phys *phys_enc,
		bool enable)
{
	struct sde_encoder_phys_cmd *cmd_enc =
		to_sde_encoder_phys_cmd(phys_enc);
	int ret = 0;
	u32 refcount;
	struct sde_kms *sde_kms;

	if (!phys_enc || !phys_enc->hw_pp) {
		SDE_ERROR("invalid encoder\n");
		return -EINVAL;
	}
	sde_kms = phys_enc->sde_kms;

	mutex_lock(phys_enc->vblank_ctl_lock);
	/* Slave encoders don't report vblank */
	if (!sde_encoder_phys_cmd_is_master(phys_enc))
		goto end;

	refcount = atomic_read(&phys_enc->vblank_refcount);

	/* protect against negative */
	if (!enable && refcount == 0) {
		ret = -EINVAL;
		goto end;
	}

	SDE_DEBUG_CMDENC(cmd_enc, "[%pS] enable=%d/%d\n",
			__builtin_return_address(0), enable, refcount);
	SDE_EVT32(DRMID(phys_enc->parent), phys_enc->hw_pp->idx - PINGPONG_0,
			enable, refcount);

	if (enable && atomic_inc_return(&phys_enc->vblank_refcount) == 1) {
		ret = sde_encoder_helper_register_irq(phys_enc, INTR_IDX_RDPTR);
		if (ret)
			atomic_dec_return(&phys_enc->vblank_refcount);
	} else if (!enable &&
			atomic_dec_return(&phys_enc->vblank_refcount) == 0) {
		ret = sde_encoder_helper_unregister_irq(phys_enc,
				INTR_IDX_RDPTR);
		if (ret)
			atomic_inc_return(&phys_enc->vblank_refcount);
	}

end:
	mutex_unlock(phys_enc->vblank_ctl_lock);
	if (ret) {
		SDE_ERROR_CMDENC(cmd_enc,
				"control vblank irq error %d, enable %d, refcount %d\n",
				ret, enable, refcount);
		SDE_EVT32(DRMID(phys_enc->parent),
				phys_enc->hw_pp->idx - PINGPONG_0,
				enable, refcount, SDE_EVTLOG_ERROR);
	}

	return ret;
}

void sde_encoder_phys_cmd_irq_control(struct sde_encoder_phys *phys_enc,
		bool enable)
{
	struct sde_encoder_phys_cmd *cmd_enc;
	bool ctl_done_supported = false;

	if (!phys_enc)
		return;

	/**
	 * pingpong split slaves do not register for IRQs
	 * check old and new topologies
	 */
	if (_sde_encoder_phys_is_ppsplit_slave(phys_enc) ||
			_sde_encoder_phys_is_disabling_ppsplit_slave(phys_enc))
		return;

	cmd_enc = to_sde_encoder_phys_cmd(phys_enc);

	SDE_EVT32(DRMID(phys_enc->parent), phys_enc->hw_pp->idx - PINGPONG_0,
			enable, atomic_read(&phys_enc->vblank_refcount));

	ctl_done_supported = sde_encoder_check_ctl_done_support(phys_enc->parent);

	if (enable) {
		if (!ctl_done_supported)
			sde_encoder_helper_register_irq(phys_enc, INTR_IDX_PINGPONG);

		sde_encoder_phys_cmd_control_vblank_irq(phys_enc, true);

		if (sde_encoder_phys_cmd_is_master(phys_enc)) {
			sde_encoder_helper_register_irq(phys_enc,
					INTR_IDX_WRPTR);
			sde_encoder_helper_register_irq(phys_enc,
					INTR_IDX_AUTOREFRESH_DONE);
			if (ctl_done_supported)
				sde_encoder_helper_register_irq(phys_enc, INTR_IDX_CTL_DONE);
		}

	} else {
		if (sde_encoder_phys_cmd_is_master(phys_enc)) {
			sde_encoder_helper_unregister_irq(phys_enc,
					INTR_IDX_WRPTR);
			sde_encoder_helper_unregister_irq(phys_enc,
					INTR_IDX_AUTOREFRESH_DONE);
			if (ctl_done_supported)
				sde_encoder_helper_unregister_irq(phys_enc, INTR_IDX_CTL_DONE);
		}

		sde_encoder_phys_cmd_control_vblank_irq(phys_enc, false);

		if (!ctl_done_supported)
			sde_encoder_helper_unregister_irq(phys_enc, INTR_IDX_PINGPONG);
	}
}

#ifdef OPLUS_FEATURE_DISPLAY_ADFR
int _get_tearcheck_threshold(struct sde_encoder_phys *phys_enc)
#else
static int _get_tearcheck_threshold(struct sde_encoder_phys *phys_enc)
#endif /* OPLUS_FEATURE_DISPLAY_ADFR */
{
	struct drm_connector *conn = phys_enc->connector;
	u32 qsync_mode;
	struct drm_display_mode *mode;
	u32 threshold_lines, adjusted_threshold_lines;
	struct sde_encoder_phys_cmd *cmd_enc =
			to_sde_encoder_phys_cmd(phys_enc);
	struct sde_encoder_virt *sde_enc;
	struct msm_mode_info *info;

	if (!conn || !conn->state)
		return 0;

	sde_enc = to_sde_encoder_virt(phys_enc->parent);
	info = &sde_enc->mode_info;
	mode = &phys_enc->cached_mode;
	qsync_mode = sde_connector_get_qsync_mode(conn);
	threshold_lines = adjusted_threshold_lines = DEFAULT_TEARCHECK_SYNC_THRESH_START;

#ifdef OPLUS_FEATURE_DISPLAY_ADFR
	OPLUS_ADFR_TRACE_BEGIN("_get_tearcheck_threshold");
#endif /* OPLUS_FEATURE_DISPLAY_ADFR */

	if (mode && (qsync_mode == SDE_RM_QSYNC_CONTINUOUS_MODE)) {
		u32 qsync_min_fps = 0;
		ktime_t qsync_time_ns;
		ktime_t qsync_l_bound_ns, qsync_u_bound_ns;
		u32 default_fps = drm_mode_vrefresh(mode);
		ktime_t default_time_ns;
		ktime_t default_line_time_ns;
		ktime_t extra_time_ns;
		u32 yres = mode->vtotal;

		if (phys_enc->parent_ops.get_qsync_fps)
			phys_enc->parent_ops.get_qsync_fps(phys_enc->parent, &qsync_min_fps,
					conn->state);


#ifdef OPLUS_FEATURE_DISPLAY_ADFR
		if (oplus_adfr_get_osync_window_min_fps(conn) >= 0) {
			qsync_min_fps = oplus_adfr_get_osync_window_min_fps(conn);
		}
#endif /* OPLUS_FEATURE_DISPLAY_ADFR */

		if (!qsync_min_fps || !default_fps || !yres) {
#ifdef OPLUS_FEATURE_DISPLAY_ADFR
			SDE_DEBUG_CMDENC(cmd_enc,
				"wrong qsync params %d %d %d\n",
				qsync_min_fps, default_fps, yres);
#else
			SDE_ERROR_CMDENC(cmd_enc,
				"wrong qsync params %d %d %d\n",
				qsync_min_fps, default_fps, yres);
#endif /* OPLUS_FEATURE_DISPLAY_ADFR */
			goto exit;
		}

		if (qsync_min_fps >= default_fps) {
			SDE_ERROR_CMDENC(cmd_enc,
				"qsync fps:%d must be less than default:%d\n",
				qsync_min_fps, default_fps);
			goto exit;
		}

		/*
		 * Calculate safe qsync trigger window by compensating
		 * the qsync timeout period by panel jitter value.
		 *
		 * qsync_safe_window_period = qsync_timeout_period * (1 - jitter) - nominal_period
		 * nominal_line_time = nominal_period / vtotal
		 * qsync_safe_window_lines = qsync_safe_window_period / nominal_line_time
		 */
		qsync_time_ns = mult_frac(1000000000, 1, qsync_min_fps);
		default_time_ns = mult_frac(1000000000, 1, default_fps);

		sde_encoder_helper_get_jitter_bounds_ns(qsync_min_fps, info->jitter_numer,
				info->jitter_denom, &qsync_l_bound_ns, &qsync_u_bound_ns);
		if (!qsync_l_bound_ns || !qsync_u_bound_ns)
			qsync_l_bound_ns = qsync_u_bound_ns = qsync_time_ns;

		extra_time_ns = qsync_l_bound_ns - default_time_ns;
		default_line_time_ns = mult_frac(1, default_time_ns, yres);
		threshold_lines = mult_frac(1, extra_time_ns, default_line_time_ns);

		/* some DDICs express the timeout value in lines/4, round down to compensate */
		adjusted_threshold_lines = round_down(threshold_lines, 4);
		/* remove 2 lines to cover for latency */
		if (adjusted_threshold_lines - 2 > DEFAULT_TEARCHECK_SYNC_THRESH_START)
			adjusted_threshold_lines -= 2;

#ifdef OPLUS_FEATURE_DISPLAY_ADFR
		oplus_adfr_osync_threshold_lines_update(conn, &adjusted_threshold_lines, yres);
#endif /* OPLUS_FEATURE_DISPLAY_ADFR */

		SDE_DEBUG_CMDENC(cmd_enc,
			"qsync mode:%u min_fps:%u time:%lld low:%lld up:%lld jitter:%u/%u\n",
			qsync_mode, qsync_min_fps, qsync_time_ns, qsync_l_bound_ns,
			qsync_u_bound_ns, info->jitter_numer, info->jitter_denom);
		SDE_DEBUG_CMDENC(cmd_enc,
			"default fps:%u time:%lld yres:%u line_time:%lld\n",
			default_fps, default_time_ns, yres, default_line_time_ns);
		SDE_DEBUG_CMDENC(cmd_enc,
			"extra_time:%lld  threshold_lines:%u adjusted_threshold_lines:%u\n",
			extra_time_ns, threshold_lines, adjusted_threshold_lines);

		SDE_EVT32(qsync_mode, qsync_min_fps, default_fps, info->jitter_numer,
				info->jitter_denom, yres, extra_time_ns, default_line_time_ns,
				adjusted_threshold_lines);
	}

exit:

#ifdef OPLUS_FEATURE_DISPLAY_ADFR
	SDE_DEBUG_CMDENC(cmd_enc, "osync_mode:%u,osync_window_min_fps:%u,threshold_lines:%u\n",
								qsync_mode, oplus_adfr_get_osync_window_min_fps(conn), adjusted_threshold_lines);
	OPLUS_ADFR_TRACE_INT("oplus_adfr_threshold_lines", adjusted_threshold_lines);
	OPLUS_ADFR_TRACE_END("_get_tearcheck_threshold");
#endif /* OPLUS_FEATURE_DISPLAY_ADFR */

	return adjusted_threshold_lines;
}

static void sde_encoder_phys_cmd_tearcheck_config(
		struct sde_encoder_phys *phys_enc)
{
	struct sde_encoder_phys_cmd *cmd_enc =
		to_sde_encoder_phys_cmd(phys_enc);
	struct sde_hw_tear_check tc_cfg = { 0 };
	struct drm_display_mode *mode;
	bool tc_enable = true;
	u32 vsync_hz;
	int vrefresh;
	struct msm_drm_private *priv;
	struct sde_kms *sde_kms;

	if (!phys_enc || !phys_enc->hw_pp || !phys_enc->hw_intf) {
		SDE_ERROR("invalid encoder\n");
		return;
	}
	mode = &phys_enc->cached_mode;

	SDE_DEBUG_CMDENC(cmd_enc, "pp %d, intf %d\n",
			phys_enc->hw_pp->idx - PINGPONG_0,
			phys_enc->hw_intf->idx - INTF_0);

	if (phys_enc->has_intf_te) {
		if (!phys_enc->hw_intf->ops.setup_tearcheck ||
			!phys_enc->hw_intf->ops.enable_tearcheck) {
			SDE_DEBUG_CMDENC(cmd_enc, "tearcheck not supported\n");
			return;
		}
	} else {
		if (!phys_enc->hw_pp->ops.setup_tearcheck ||
			!phys_enc->hw_pp->ops.enable_tearcheck) {
			SDE_DEBUG_CMDENC(cmd_enc, "tearcheck not supported\n");
			return;
		}
	}

	sde_kms = phys_enc->sde_kms;
	if (!sde_kms || !sde_kms->dev || !sde_kms->dev->dev_private) {
		SDE_ERROR("invalid device\n");
		return;
	}
	priv = sde_kms->dev->dev_private;

	vrefresh = drm_mode_vrefresh(mode);
	/*
	 * TE default: dsi byte clock calculated base on 70 fps;
	 * around 14 ms to complete a kickoff cycle if te disabled;
	 * vclk_line base on 60 fps; write is faster than read;
	 * init == start == rdptr;
	 *
	 * vsync_count is ratio of MDP VSYNC clock frequency to LCD panel
	 * frequency divided by the no. of rows (lines) in the LCDpanel.
	 */
	vsync_hz = sde_power_clk_get_rate(&priv->phandle, "vsync_clk");
	if (!vsync_hz || !mode->vtotal || !vrefresh) {
		SDE_DEBUG_CMDENC(cmd_enc,
			"invalid params - vsync_hz %u vtot %u vrefresh %u\n",
			vsync_hz, mode->vtotal, vrefresh);
		return;
	}

	tc_cfg.vsync_count = vsync_hz / (mode->vtotal * vrefresh);

	/* enable external TE after kickoff to avoid premature autorefresh */
	tc_cfg.hw_vsync_mode = 0;

	/*
	 * By setting sync_cfg_height to near max register value, we essentially
	 * disable sde hw generated TE signal, since hw TE will arrive first.
	 * Only caveat is if due to error, we hit wrap-around.
	 */
	tc_cfg.sync_cfg_height = 0xFFF0;
	tc_cfg.vsync_init_val = mode->vdisplay;
	tc_cfg.sync_threshold_start = _get_tearcheck_threshold(phys_enc);
	tc_cfg.sync_threshold_continue = DEFAULT_TEARCHECK_SYNC_THRESH_CONTINUE;
	tc_cfg.start_pos = mode->vdisplay;
	tc_cfg.rd_ptr_irq = mode->vdisplay + 1;
	tc_cfg.wr_ptr_irq = 1;
	cmd_enc->qsync_threshold_lines = tc_cfg.sync_threshold_start;

	SDE_DEBUG_CMDENC(cmd_enc,
	  "tc %d intf %d vsync_clk_speed_hz %u vtotal %u vrefresh %u\n",
		phys_enc->hw_pp->idx - PINGPONG_0,
		phys_enc->hw_intf->idx - INTF_0,
		vsync_hz, mode->vtotal, vrefresh);
	SDE_DEBUG_CMDENC(cmd_enc,
	  "tc %d intf %d enable %u start_pos %u rd_ptr_irq %u wr_ptr_irq %u\n",
		phys_enc->hw_pp->idx - PINGPONG_0,
		phys_enc->hw_intf->idx - INTF_0,
		tc_enable, tc_cfg.start_pos, tc_cfg.rd_ptr_irq,
		tc_cfg.wr_ptr_irq);
	SDE_DEBUG_CMDENC(cmd_enc,
	  "tc %d intf %d hw_vsync_mode %u vsync_count %u vsync_init_val %u\n",
		phys_enc->hw_pp->idx - PINGPONG_0,
		phys_enc->hw_intf->idx - INTF_0,
		tc_cfg.hw_vsync_mode, tc_cfg.vsync_count,
		tc_cfg.vsync_init_val);
	SDE_DEBUG_CMDENC(cmd_enc,
	  "tc %d intf %d cfgheight %u thresh_start %u thresh_cont %u\n",
		phys_enc->hw_pp->idx - PINGPONG_0,
		phys_enc->hw_intf->idx - INTF_0,
		tc_cfg.sync_cfg_height,
		tc_cfg.sync_threshold_start, tc_cfg.sync_threshold_continue);

	SDE_EVT32(phys_enc->hw_pp->idx - PINGPONG_0, phys_enc->hw_intf->idx - INTF_0,
			vsync_hz, mode->vtotal, vrefresh);
	SDE_EVT32(tc_enable, tc_cfg.start_pos, tc_cfg.rd_ptr_irq, tc_cfg.wr_ptr_irq,
			tc_cfg.hw_vsync_mode, tc_cfg.vsync_count, tc_cfg.vsync_init_val,
			tc_cfg.sync_cfg_height, tc_cfg.sync_threshold_start,
			tc_cfg.sync_threshold_continue);

	if (phys_enc->has_intf_te) {
		phys_enc->hw_intf->ops.setup_tearcheck(phys_enc->hw_intf,
				&tc_cfg);
		phys_enc->hw_intf->ops.enable_tearcheck(phys_enc->hw_intf,
				tc_enable);
	} else {
		phys_enc->hw_pp->ops.setup_tearcheck(phys_enc->hw_pp, &tc_cfg);
		phys_enc->hw_pp->ops.enable_tearcheck(phys_enc->hw_pp,
				tc_enable);
	}
}

static void _sde_encoder_phys_cmd_pingpong_config(
		struct sde_encoder_phys *phys_enc)
{
	struct sde_encoder_phys_cmd *cmd_enc =
		to_sde_encoder_phys_cmd(phys_enc);

	if (!phys_enc || !phys_enc->hw_ctl || !phys_enc->hw_pp) {
		SDE_ERROR("invalid arg(s), enc %d\n", !phys_enc);
		return;
	}

	SDE_DEBUG_CMDENC(cmd_enc, "pp %d, enabling mode:\n",
			phys_enc->hw_pp->idx - PINGPONG_0);
	drm_mode_debug_printmodeline(&phys_enc->cached_mode);

	if (!_sde_encoder_phys_is_ppsplit_slave(phys_enc))
		_sde_encoder_phys_cmd_update_intf_cfg(phys_enc);
	sde_encoder_phys_cmd_tearcheck_config(phys_enc);
}

static void sde_encoder_phys_cmd_enable_helper(
		struct sde_encoder_phys *phys_enc)
{
	struct sde_encoder_virt *sde_enc;
	struct sde_hw_intf *hw_intf;
	u32 qsync_mode;

	if (!phys_enc || !phys_enc->hw_ctl || !phys_enc->hw_pp ||
			!phys_enc->hw_intf) {
		SDE_ERROR("invalid arg(s), encoder %d\n", !phys_enc);
		return;
	}

	sde_encoder_helper_split_config(phys_enc, phys_enc->intf_idx);

	_sde_encoder_phys_cmd_pingpong_config(phys_enc);

	hw_intf = phys_enc->hw_intf;
	if (hw_intf->ops.enable_compressed_input)
		hw_intf->ops.enable_compressed_input(phys_enc->hw_intf,
				(phys_enc->comp_type !=
				 MSM_DISPLAY_COMPRESSION_NONE), false);

	if (hw_intf->ops.enable_wide_bus)
		hw_intf->ops.enable_wide_bus(hw_intf,
			sde_encoder_is_widebus_enabled(phys_enc->parent));

	/*
	 * Override internal rd_ptr value when coming out of IPC.
	 * This is required on QSYNC panel with low refresh rate to
	 * avoid out of sync frame trigger as panel rd_ptr was still
	 * incrementing while MDP was power collapsed.
	 */
	sde_enc = to_sde_encoder_virt(phys_enc->parent);
	if (sde_enc->idle_pc_restore) {
		qsync_mode = sde_connector_get_qsync_mode(phys_enc->connector);
		if (qsync_mode)
			sde_encoder_override_tearcheck_rd_ptr(phys_enc);
	}

	/*
	 * For pp-split, skip setting the flush bit for the slave intf, since
	 * both intfs use same ctl and HW will only flush the master.
	 */
	if (_sde_encoder_phys_is_ppsplit(phys_enc) &&
		!sde_encoder_phys_cmd_is_master(phys_enc))
		goto skip_flush;

	_sde_encoder_phys_cmd_update_flush_mask(phys_enc);

skip_flush:
	return;
}

static void sde_encoder_phys_cmd_enable(struct sde_encoder_phys *phys_enc)
{
	struct sde_encoder_phys_cmd *cmd_enc =
		to_sde_encoder_phys_cmd(phys_enc);

	if (!phys_enc || !phys_enc->hw_pp) {
		SDE_ERROR("invalid phys encoder\n");
		return;
	}

	SDE_DEBUG_CMDENC(cmd_enc, "pp %d\n", phys_enc->hw_pp->idx - PINGPONG_0);

	if (phys_enc->enable_state == SDE_ENC_ENABLED) {
		if (!phys_enc->cont_splash_enabled)
			SDE_ERROR("already enabled\n");
		return;
	}

	sde_encoder_phys_cmd_enable_helper(phys_enc);
	phys_enc->enable_state = SDE_ENC_ENABLED;
}

static bool sde_encoder_phys_cmd_is_autorefresh_enabled(
		struct sde_encoder_phys *phys_enc)
{
	struct sde_hw_pingpong *hw_pp;
	struct sde_hw_intf *hw_intf;
	struct sde_hw_autorefresh cfg;
	int ret;

	if (!phys_enc || !phys_enc->hw_pp || !phys_enc->hw_intf)
		return false;

	if (!sde_encoder_phys_cmd_is_master(phys_enc))
		return false;

	if (phys_enc->has_intf_te) {
		hw_intf = phys_enc->hw_intf;
		if (!hw_intf->ops.get_autorefresh)
			return false;

		ret = hw_intf->ops.get_autorefresh(hw_intf, &cfg);
	} else {
		hw_pp = phys_enc->hw_pp;
		if (!hw_pp->ops.get_autorefresh)
			return false;

		ret = hw_pp->ops.get_autorefresh(hw_pp, &cfg);
	}

	return ret ? false : cfg.enable;
}

static void sde_encoder_phys_cmd_connect_te(
		struct sde_encoder_phys *phys_enc, bool enable)
{
	if (!phys_enc || !phys_enc->hw_pp || !phys_enc->hw_intf)
		return;

	if (phys_enc->has_intf_te &&
			phys_enc->hw_intf->ops.connect_external_te)
		phys_enc->hw_intf->ops.connect_external_te(phys_enc->hw_intf,
				enable);
	else if (phys_enc->hw_pp->ops.connect_external_te)
		phys_enc->hw_pp->ops.connect_external_te(phys_enc->hw_pp,
				enable);
	else
		return;

	SDE_EVT32(DRMID(phys_enc->parent), enable);
}

static int sde_encoder_phys_cmd_te_get_line_count(
		struct sde_encoder_phys *phys_enc)
{
	struct sde_hw_pingpong *hw_pp;
	struct sde_hw_intf *hw_intf;
	u32 line_count;

	if (!phys_enc || !phys_enc->hw_pp || !phys_enc->hw_intf)
		return -EINVAL;

	if (!sde_encoder_phys_cmd_is_master(phys_enc))
		return -EINVAL;

	if (phys_enc->has_intf_te) {
		hw_intf = phys_enc->hw_intf;
		if (!hw_intf->ops.get_line_count)
			return -EINVAL;

		line_count = hw_intf->ops.get_line_count(hw_intf);
	} else {
		hw_pp = phys_enc->hw_pp;
		if (!hw_pp->ops.get_line_count)
			return -EINVAL;

		line_count = hw_pp->ops.get_line_count(hw_pp);
	}

	return line_count;
}

static void sde_encoder_phys_cmd_disable(struct sde_encoder_phys *phys_enc)
{
	struct sde_encoder_phys_cmd *cmd_enc =
		to_sde_encoder_phys_cmd(phys_enc);

	if (!phys_enc || !phys_enc->hw_pp || !phys_enc->hw_intf) {
		SDE_ERROR("invalid encoder\n");
		return;
	}
	SDE_DEBUG_CMDENC(cmd_enc, "pp %d intf %d state %d\n",
			phys_enc->hw_pp->idx - PINGPONG_0,
			phys_enc->hw_intf->idx - INTF_0,
			phys_enc->enable_state);
	SDE_EVT32(DRMID(phys_enc->parent), phys_enc->hw_pp->idx - PINGPONG_0,
			phys_enc->hw_intf->idx - INTF_0,
			phys_enc->enable_state);

	if (phys_enc->enable_state == SDE_ENC_DISABLED) {
		SDE_ERROR_CMDENC(cmd_enc, "already disabled\n");
		return;
	}

	if (!sde_in_trusted_vm(phys_enc->sde_kms)) {
		if (phys_enc->has_intf_te &&
				phys_enc->hw_intf->ops.enable_tearcheck)
			phys_enc->hw_intf->ops.enable_tearcheck(
					phys_enc->hw_intf,
					false);
		else if (phys_enc->hw_pp->ops.enable_tearcheck)
			phys_enc->hw_pp->ops.enable_tearcheck(phys_enc->hw_pp,
					false);
		if (sde_encoder_phys_cmd_is_master(phys_enc))
			sde_encoder_helper_phys_disable(phys_enc, NULL);
		if (phys_enc->hw_intf->ops.reset_counter)
			phys_enc->hw_intf->ops.reset_counter(phys_enc->hw_intf);
	}

	memset(&cmd_enc->autorefresh.cfg, 0, sizeof(struct sde_hw_autorefresh));
	phys_enc->enable_state = SDE_ENC_DISABLED;
}

static void sde_encoder_phys_cmd_destroy(struct sde_encoder_phys *phys_enc)
{
	struct sde_encoder_phys_cmd *cmd_enc =
		to_sde_encoder_phys_cmd(phys_enc);

	if (!phys_enc) {
		SDE_ERROR("invalid encoder\n");
		return;
	}
	kfree(cmd_enc);
}

static void sde_encoder_phys_cmd_get_hw_resources(
		struct sde_encoder_phys *phys_enc,
		struct sde_encoder_hw_resources *hw_res,
		struct drm_connector_state *conn_state)
{
	struct sde_encoder_phys_cmd *cmd_enc =
		to_sde_encoder_phys_cmd(phys_enc);

	if (!phys_enc) {
		SDE_ERROR("invalid encoder\n");
		return;
	}

	if ((phys_enc->intf_idx - INTF_0) >= INTF_MAX) {
		SDE_ERROR("invalid intf idx:%d\n", phys_enc->intf_idx);
		return;
	}

	SDE_DEBUG_CMDENC(cmd_enc, "\n");
	hw_res->intfs[phys_enc->intf_idx - INTF_0] = INTF_MODE_CMD;
}

static void _sde_encoder_phys_wait_for_vsync_on_autorefresh_busy(struct sde_encoder_phys *phys_enc)
{
	u32 autorefresh_status;
	int ret = 0;

	if (!phys_enc || !phys_enc->hw_intf || !phys_enc->hw_intf->ops.get_autorefresh_status) {
		SDE_ERROR("invalid params\n");
		return;
	}

	autorefresh_status = phys_enc->hw_intf->ops.get_autorefresh_status(phys_enc->hw_intf);
	if (autorefresh_status) {
		ret = sde_encoder_wait_for_event(phys_enc->parent, MSM_ENC_VBLANK);
		if (ret) {
			autorefresh_status = phys_enc->hw_intf->ops.get_autorefresh_status(
					phys_enc->hw_intf);
			SDE_ERROR("wait for vblank timed out, autorefresh_status:%d\n",
					autorefresh_status);
		}
	}
}

static int sde_encoder_phys_cmd_prepare_for_kickoff(
		struct sde_encoder_phys *phys_enc,
		struct sde_encoder_kickoff_params *params)
{
	struct sde_hw_tear_check tc_cfg = {0};
	struct sde_encoder_virt *sde_enc;
	struct sde_encoder_phys_cmd *cmd_enc =
			to_sde_encoder_phys_cmd(phys_enc);
	int ret = 0;
	bool recovery_events;

	if (!phys_enc || !phys_enc->hw_pp) {
		SDE_ERROR("invalid encoder\n");
		return -EINVAL;
	}
	SDE_DEBUG_CMDENC(cmd_enc, "pp %d\n", phys_enc->hw_pp->idx - PINGPONG_0);

	phys_enc->frame_trigger_mode = params->frame_trigger_mode;
	sde_enc = to_sde_encoder_virt(phys_enc->parent);
	SDE_EVT32(DRMID(phys_enc->parent), phys_enc->hw_pp->idx - PINGPONG_0,
			atomic_read(&phys_enc->pending_kickoff_cnt),
			atomic_read(&cmd_enc->autorefresh.kickoff_cnt),
			phys_enc->frame_trigger_mode, phys_enc->cont_splash_enabled);

	if (phys_enc->frame_trigger_mode == FRAME_DONE_WAIT_DEFAULT) {
		/*
		 * Mark kickoff request as outstanding. If there are more
		 * than one outstanding frame, then we have to wait for the
		 * previous frame to complete
		 */
		ret = _sde_encoder_phys_cmd_wait_for_idle(phys_enc);
		if (ret) {
			atomic_set(&phys_enc->pending_kickoff_cnt, 0);
			SDE_EVT32(DRMID(phys_enc->parent),
					phys_enc->hw_pp->idx - PINGPONG_0);
			SDE_ERROR("failed wait_for_idle: %d\n", ret);
		}
	}

	if (phys_enc->cont_splash_enabled)
		_sde_encoder_phys_wait_for_vsync_on_autorefresh_busy(phys_enc);

	if (phys_enc->recovered) {
		recovery_events = sde_encoder_recovery_events_enabled(
				phys_enc->parent);
		if (cmd_enc->frame_tx_timeout_report_cnt && recovery_events)
			sde_connector_event_notify(phys_enc->connector,
					DRM_EVENT_SDE_HW_RECOVERY,
					sizeof(uint8_t),
					SDE_RECOVERY_SUCCESS);

		cmd_enc->frame_tx_timeout_report_cnt = 0;
		phys_enc->recovered = false;
	}

#ifdef OPLUS_FEATURE_DISPLAY_ADFR
	oplus_adfr_force_off_osync_mode(phys_enc);
	if (oplus_adfr_osync_tearcheck_update(phys_enc) != -ENOTSUPP) {
		SDE_DEBUG_CMDENC(cmd_enc, "use custom function\n");
	} else {
#endif /* OPLUS_FEATURE_DISPLAY_ADFR */
	if (sde_connector_is_qsync_updated(phys_enc->connector)) {
		tc_cfg.sync_threshold_start = _get_tearcheck_threshold(
				phys_enc);
		cmd_enc->qsync_threshold_lines = tc_cfg.sync_threshold_start;
		if (phys_enc->has_intf_te &&
				phys_enc->hw_intf->ops.update_tearcheck)
			phys_enc->hw_intf->ops.update_tearcheck(
					phys_enc->hw_intf, &tc_cfg);
		else if (phys_enc->hw_pp->ops.update_tearcheck)
			phys_enc->hw_pp->ops.update_tearcheck(
					phys_enc->hw_pp, &tc_cfg);
		SDE_EVT32(DRMID(phys_enc->parent), tc_cfg.sync_threshold_start);
	}

#ifdef OPLUS_FEATURE_DISPLAY_ADFR
	}

	oplus_adfr_adjust_osync_tearcheck(phys_enc);
#endif /* OPLUS_FEATURE_DISPLAY_ADFR */

	SDE_DEBUG_CMDENC(cmd_enc, "pp:%d pending_cnt %d\n",
			phys_enc->hw_pp->idx - PINGPONG_0,
			atomic_read(&phys_enc->pending_kickoff_cnt));
	return ret;
}

static bool _sde_encoder_phys_cmd_needs_vsync_change(
		struct sde_encoder_phys *phys_enc, ktime_t profile_timestamp)
{
	struct sde_encoder_virt *sde_enc;
	struct sde_encoder_phys_cmd *cmd_enc;
	struct sde_encoder_phys_cmd_te_timestamp *cur;
	struct sde_encoder_phys_cmd_te_timestamp *prev = NULL;
	ktime_t time_diff;
	struct msm_mode_info *info;
	ktime_t l_bound = 0, u_bound = 0;
	bool ret = false;
	unsigned long lock_flags;

	cmd_enc = to_sde_encoder_phys_cmd(phys_enc);
	sde_enc = to_sde_encoder_virt(phys_enc->parent);
	info = &sde_enc->mode_info;

	sde_encoder_helper_get_jitter_bounds_ns(info->frame_rate, info->jitter_numer,
			info->jitter_denom, &l_bound, &u_bound);
	if (!l_bound || !u_bound) {
		SDE_ERROR_CMDENC(cmd_enc, "invalid vsync jitter bounds\n");
		return false;
	}

	spin_lock_irqsave(phys_enc->enc_spinlock, lock_flags);
	list_for_each_entry_reverse(cur, &cmd_enc->te_timestamp_list, list) {
		if (prev && ktime_after(cur->timestamp, profile_timestamp)) {
			time_diff = ktime_sub(prev->timestamp, cur->timestamp);
			if ((time_diff < l_bound) || (time_diff > u_bound)) {
				ret = true;
				break;
			}
		}
		prev = cur;
	}
	spin_unlock_irqrestore(phys_enc->enc_spinlock, lock_flags);

	if (ret) {
		SDE_DEBUG_CMDENC(cmd_enc,
		    "time_diff:%llu, prev:%llu, cur:%llu, jitter:%llu/%llu\n",
			time_diff, prev->timestamp, cur->timestamp,
			l_bound, u_bound);
		time_diff = div_s64(time_diff, 1000);

		SDE_EVT32(DRMID(phys_enc->parent),
			(u32) (do_div(l_bound, 1000)),
			(u32) (do_div(u_bound, 1000)),
			(u32) (time_diff), SDE_EVTLOG_ERROR);
	}

	return ret;
}

static int _sde_encoder_phys_cmd_wait_for_wr_ptr(
		struct sde_encoder_phys *phys_enc)
{
	struct sde_encoder_phys_cmd *cmd_enc =
			to_sde_encoder_phys_cmd(phys_enc);
	struct sde_encoder_wait_info wait_info = {0};
	struct sde_connector *c_conn;
	bool frame_pending = true;
	struct sde_hw_ctl *ctl;
	unsigned long lock_flags;
	int ret, timeout_ms;

	if (!phys_enc || !phys_enc->hw_ctl || !phys_enc->connector) {
		SDE_ERROR("invalid argument(s)\n");
		return -EINVAL;
	}
	ctl = phys_enc->hw_ctl;
	c_conn = to_sde_connector(phys_enc->connector);
	timeout_ms = phys_enc->kickoff_timeout_ms;

	if (c_conn->lp_mode == SDE_MODE_DPMS_LP1 ||
		c_conn->lp_mode == SDE_MODE_DPMS_LP2)
		timeout_ms = timeout_ms * 2;

	wait_info.wq = &phys_enc->pending_kickoff_wq;
	wait_info.atomic_cnt = &phys_enc->pending_retire_fence_cnt;
	wait_info.timeout_ms = timeout_ms;

	/* slave encoder doesn't enable for ppsplit */
	if (_sde_encoder_phys_is_ppsplit_slave(phys_enc))
		return 0;

	ret = sde_encoder_helper_wait_for_irq(phys_enc, INTR_IDX_WRPTR,
			&wait_info);

	/*
	 * if hwfencing enabled, try again to wait for up to the extended timeout time in
	 * increments as long as fence has not been signaled.
	 */
	if (ret == -ETIMEDOUT && phys_enc->sde_kms->catalog->hw_fence_rev)
		ret = sde_encoder_helper_hw_fence_extended_wait(phys_enc, ctl, &wait_info,
			INTR_IDX_WRPTR);

	if (ret == -ETIMEDOUT) {
		struct sde_hw_ctl *ctl = phys_enc->hw_ctl;

		if (ctl && ctl->ops.get_start_state)
			frame_pending = ctl->ops.get_start_state(ctl);

		ret = (frame_pending || sde_connector_esd_status(phys_enc->connector)) ? ret : 0;

		/*
		 * There can be few cases of ESD where CTL_START is cleared but
		 * wr_ptr irq doesn't come. Signaling retire fence in these
		 * cases to avoid freeze and dangling pending_retire_fence_cnt
		 */
		if (!ret) {
			SDE_EVT32(DRMID(phys_enc->parent),
				SDE_EVTLOG_FUNC_CASE1);

			if (sde_encoder_phys_cmd_is_master(phys_enc) &&
				atomic_add_unless(
				&phys_enc->pending_retire_fence_cnt, -1, 0)) {
				spin_lock_irqsave(phys_enc->enc_spinlock,
					lock_flags);
				phys_enc->parent_ops.handle_frame_done(
				 phys_enc->parent, phys_enc,
				 SDE_ENCODER_FRAME_EVENT_SIGNAL_RETIRE_FENCE);
				spin_unlock_irqrestore(phys_enc->enc_spinlock,
					lock_flags);
			}
		}

		/* if we timeout after the extended wait, reset mixers and do sw override */
		if (ret && phys_enc->sde_kms->catalog->hw_fence_rev)
			sde_encoder_helper_hw_fence_sw_override(phys_enc, ctl);
	}

	cmd_enc->wr_ptr_wait_success = (ret == 0) ? true : false;
	return ret;
}

static int sde_encoder_phys_cmd_wait_for_tx_complete(
		struct sde_encoder_phys *phys_enc)
{
	int rc;
	struct sde_encoder_phys_cmd *cmd_enc;

	if (!phys_enc)
		return -EINVAL;

	cmd_enc = to_sde_encoder_phys_cmd(phys_enc);

	if (sde_encoder_check_ctl_done_support(phys_enc->parent)
			&& !sde_encoder_phys_cmd_is_master(phys_enc))
		return 0;

	if (!atomic_read(&phys_enc->pending_kickoff_cnt)) {
		SDE_EVT32(DRMID(phys_enc->parent),
			phys_enc->intf_idx - INTF_0,
			phys_enc->enable_state);
		return 0;
	}

	rc = _sde_encoder_phys_cmd_wait_for_idle(phys_enc);
	if (rc) {
		SDE_EVT32(DRMID(phys_enc->parent),
				phys_enc->intf_idx - INTF_0);
		SDE_ERROR("failed wait_for_idle: %d\n", rc);
		oplus_sde_evtlog_dump_all();
	}

	return rc;
}

static int _sde_encoder_phys_cmd_handle_wr_ptr_timeout(
		struct sde_encoder_phys *phys_enc,
		ktime_t profile_timestamp)
{
	struct sde_encoder_phys_cmd *cmd_enc =
			to_sde_encoder_phys_cmd(phys_enc);
	bool switch_te;
	int ret = -ETIMEDOUT;
	unsigned long lock_flags;

	switch_te = _sde_encoder_phys_cmd_needs_vsync_change(
				phys_enc, profile_timestamp);

	SDE_EVT32(DRMID(phys_enc->parent), switch_te, SDE_EVTLOG_FUNC_ENTRY);

	if (sde_connector_panel_dead(phys_enc->connector)) {
		ret = _sde_encoder_phys_cmd_wait_for_wr_ptr(phys_enc);
	} else if (switch_te) {
		SDE_DEBUG_CMDENC(cmd_enc,
				"wr_ptr_irq wait failed, retry with WD TE\n");

		/* switch to watchdog TE and wait again */
		sde_encoder_helper_switch_vsync(phys_enc->parent, true);

		ret = _sde_encoder_phys_cmd_wait_for_wr_ptr(phys_enc);

		/* switch back to default TE */
		sde_encoder_helper_switch_vsync(phys_enc->parent, false);
	}

	/*
	 * Signaling the retire fence at wr_ptr timeout
	 * to allow the next commit and avoid device freeze.
	 */
	if (ret == -ETIMEDOUT) {
		SDE_ERROR_CMDENC(cmd_enc,
			"wr_ptr_irq wait failed, switch_te:%d\n", switch_te);
		SDE_EVT32(DRMID(phys_enc->parent), switch_te, SDE_EVTLOG_ERROR);

		if (sde_encoder_phys_cmd_is_master(phys_enc) &&
			atomic_add_unless(
			&phys_enc->pending_retire_fence_cnt, -1, 0)) {
			spin_lock_irqsave(phys_enc->enc_spinlock, lock_flags);
			phys_enc->parent_ops.handle_frame_done(
				phys_enc->parent, phys_enc,
				SDE_ENCODER_FRAME_EVENT_SIGNAL_RETIRE_FENCE);
			spin_unlock_irqrestore(phys_enc->enc_spinlock,
				lock_flags);
		}
	}

	cmd_enc->wr_ptr_wait_success = (ret == 0) ? true : false;

	return ret;
}

static int sde_encoder_phys_cmd_wait_for_commit_done(
		struct sde_encoder_phys *phys_enc)
{
	int rc = 0, i, pending_cnt;
	struct sde_encoder_phys_cmd *cmd_enc;
	ktime_t profile_timestamp = ktime_get();
	u32 scheduler_status = INVALID_CTL_STATUS;
	struct sde_hw_ctl *ctl;

	if (!phys_enc)
		return -EINVAL;

	cmd_enc = to_sde_encoder_phys_cmd(phys_enc);

	if (sde_encoder_check_ctl_done_support(phys_enc->parent)
			&& !sde_encoder_phys_cmd_is_master(phys_enc))
		return 0;

	/* only required for master controller */
	if (sde_encoder_phys_cmd_is_master(phys_enc)) {
		rc = _sde_encoder_phys_cmd_wait_for_wr_ptr(phys_enc);
		if (rc == -ETIMEDOUT) {
			/*
			 * Profile all the TE received after profile_timestamp
			 * and if the jitter is more, switch to watchdog TE
			 * and wait for wr_ptr again. Finally move back to
			 * default TE.
			 */
			rc = _sde_encoder_phys_cmd_handle_wr_ptr_timeout(
					phys_enc, profile_timestamp);
			if (rc == -ETIMEDOUT)
				goto wait_for_idle;
		}

		if (cmd_enc->autorefresh.cfg.enable)
			rc = _sde_encoder_phys_cmd_wait_for_autorefresh_done(
								phys_enc);

		ctl = phys_enc->hw_ctl;
		if (ctl && ctl->ops.get_scheduler_status)
			scheduler_status = ctl->ops.get_scheduler_status(ctl);
	}

	/* wait for posted start or serialize trigger */
	pending_cnt = atomic_read(&phys_enc->pending_kickoff_cnt);
	if ((pending_cnt > 1) ||
	    (pending_cnt && (scheduler_status & BIT(0))) ||
	    (!rc && phys_enc->frame_trigger_mode == FRAME_DONE_WAIT_SERIALIZE))
		goto wait_for_idle;

	return rc;

wait_for_idle:
	pending_cnt = atomic_read(&phys_enc->pending_kickoff_cnt);
	for (i = 0; i < pending_cnt; i++)
		rc |= sde_encoder_wait_for_event(phys_enc->parent,
				MSM_ENC_TX_COMPLETE);
	if (rc) {
		SDE_EVT32(DRMID(phys_enc->parent),
			phys_enc->hw_pp->idx - PINGPONG_0,
			phys_enc->frame_trigger_mode,
			atomic_read(&phys_enc->pending_kickoff_cnt),
			phys_enc->enable_state,
			cmd_enc->wr_ptr_wait_success, scheduler_status, rc);
		SDE_ERROR("pp:%d failed wait_for_idle: %d\n",
				phys_enc->hw_pp->idx - PINGPONG_0, rc);
		if (phys_enc->enable_state == SDE_ENC_ERR_NEEDS_HW_RESET)
			sde_encoder_needs_hw_reset(phys_enc->parent);
	}

	return rc;
}

static int sde_encoder_phys_cmd_wait_for_vblank(
		struct sde_encoder_phys *phys_enc)
{
	int rc = 0;
	struct sde_encoder_phys_cmd *cmd_enc;
	struct sde_encoder_wait_info wait_info = {0};

	if (!phys_enc)
		return -EINVAL;

	cmd_enc = to_sde_encoder_phys_cmd(phys_enc);

	/* only required for master controller */
	if (!sde_encoder_phys_cmd_is_master(phys_enc))
		return rc;

	wait_info.wq = &cmd_enc->pending_vblank_wq;
	wait_info.atomic_cnt = &cmd_enc->pending_vblank_cnt;

#ifdef OPLUS_FEATURE_DISPLAY_ONSCREENFINGERPRINT
	if (oplus_ofp_is_supported() && oplus_ofp_ultra_low_power_aod_is_enabled()
			&& oplus_ofp_get_ultra_low_power_aod_state()) {
		wait_info.timeout_ms = OPLUS_OFP_ULTRA_LOW_POWER_AOD_VBLANK_TIMEOUT_MS;
	} else {
		wait_info.timeout_ms = _sde_encoder_phys_cmd_get_idle_timeout(phys_enc);
	}
#else
	wait_info.timeout_ms = _sde_encoder_phys_cmd_get_idle_timeout(phys_enc);
#endif /* OPLUS_FEATURE_DISPLAY_ONSCREENFINGERPRINT */

	atomic_inc(&cmd_enc->pending_vblank_cnt);

	rc = sde_encoder_helper_wait_for_irq(phys_enc, INTR_IDX_RDPTR,
			&wait_info);

	return rc;
}

static void sde_encoder_phys_cmd_update_split_role(
		struct sde_encoder_phys *phys_enc,
		enum sde_enc_split_role role)
{
	struct sde_encoder_phys_cmd *cmd_enc;
	enum sde_enc_split_role old_role;
	bool is_ppsplit;

	if (!phys_enc)
		return;

	cmd_enc = to_sde_encoder_phys_cmd(phys_enc);
	old_role = phys_enc->split_role;
	is_ppsplit = _sde_encoder_phys_is_ppsplit(phys_enc);

	phys_enc->split_role = role;

	SDE_DEBUG_CMDENC(cmd_enc, "old role %d new role %d\n",
			old_role, role);

	/*
	 * ppsplit solo needs to reprogram because intf may have swapped without
	 * role changing on left-only, right-only back-to-back commits
	 */
	if (!(is_ppsplit && role == ENC_ROLE_SOLO) &&
			(role == old_role || role == ENC_ROLE_SKIP))
		return;

	sde_encoder_helper_split_config(phys_enc, phys_enc->intf_idx);
	_sde_encoder_phys_cmd_pingpong_config(phys_enc);
	_sde_encoder_phys_cmd_update_flush_mask(phys_enc);
}

static void _sde_encoder_autorefresh_disable_seq1(
		struct sde_encoder_phys *phys_enc)
{
	int trial = 0;
	u32 timeout_ms = phys_enc->kickoff_timeout_ms;
	struct sde_encoder_phys_cmd *cmd_enc =
				to_sde_encoder_phys_cmd(phys_enc);

	/*
	 * If autorefresh is enabled, disable it and make sure it is safe to
	 * proceed with current frame commit/push. Sequence fallowed is,
	 * 1. Disable TE & autorefresh - caller will take care of it
	 * 2. Poll for frame transfer ongoing to be false
	 * 3. Enable TE back - caller will take care of it
	 */
	do {
		udelay(AUTOREFRESH_SEQ1_POLL_TIME);
		if ((trial * AUTOREFRESH_SEQ1_POLL_TIME)
				> (timeout_ms * USEC_PER_MSEC)) {
			SDE_ERROR_CMDENC(cmd_enc,
					"disable autorefresh failed\n");

			phys_enc->enable_state = SDE_ENC_ERR_NEEDS_HW_RESET;
			break;
		}

		trial++;
	} while (_sde_encoder_phys_cmd_is_ongoing_pptx(phys_enc));
}
#if defined(PXLW_IRIS_DUAL)
#include "sde_iris_encoder_phys_cmd.c"
#endif

static void _sde_encoder_autorefresh_disable_seq2(
		struct sde_encoder_phys *phys_enc)
{
	int trial = 0;
	struct sde_hw_mdp *hw_mdp = phys_enc->hw_mdptop;
	u32 autorefresh_status = 0;
	struct sde_encoder_phys_cmd *cmd_enc =
				to_sde_encoder_phys_cmd(phys_enc);
	struct intf_tear_status tear_status;
	struct sde_hw_intf *hw_intf = phys_enc->hw_intf;

	if (!hw_mdp->ops.get_autorefresh_status ||
			!hw_intf->ops.check_and_reset_tearcheck) {
		SDE_DEBUG_CMDENC(cmd_enc,
			"autofresh disable seq2 not supported\n");
		return;
	}

	/*
	 * If autorefresh is still enabled after sequence-1, proceed with
	 * below sequence-2.
	 * 1. Disable autorefresh config
	 * 2. Run in loop:
	 *    2.1 Poll for autorefresh to be disabled
	 *    2.2 Log read and write count status
	 *    2.3 Replace te write count with start_pos to meet trigger window
	 */
	autorefresh_status = hw_mdp->ops.get_autorefresh_status(hw_mdp,
					phys_enc->intf_idx);
	SDE_EVT32(DRMID(phys_enc->parent), phys_enc->intf_idx - INTF_0,
				autorefresh_status, SDE_EVTLOG_FUNC_CASE1);

#if defined(PXLW_IRIS_DUAL)
	/* donot get autorefresh_status second time for 120hz case */
	if (!(autorefresh_status & BIT(7)) && !iris_is_dual_supported()) {
#else
	if (!(autorefresh_status & BIT(7))) {
#endif
		usleep_range(AUTOREFRESH_SEQ2_POLL_TIME,
			AUTOREFRESH_SEQ2_POLL_TIME + 1);

		autorefresh_status = hw_mdp->ops.get_autorefresh_status(hw_mdp,
					phys_enc->intf_idx);
		SDE_EVT32(DRMID(phys_enc->parent), phys_enc->intf_idx - INTF_0,
				autorefresh_status, SDE_EVTLOG_FUNC_CASE2);
	}

	while (autorefresh_status & BIT(7)) {
		if (!trial) {
			pr_err("enc:%d autofresh status:0x%x intf:%d\n", DRMID(phys_enc->parent),
					autorefresh_status, phys_enc->intf_idx - INTF_0);

			_sde_encoder_phys_cmd_config_autorefresh(phys_enc, 0);
		}

		usleep_range(AUTOREFRESH_SEQ2_POLL_TIME,
				AUTOREFRESH_SEQ2_POLL_TIME + 1);
		if ((trial * AUTOREFRESH_SEQ2_POLL_TIME)
			> AUTOREFRESH_SEQ2_POLL_TIMEOUT) {
			SDE_ERROR_CMDENC(cmd_enc,
					"disable autorefresh failed\n");
			SDE_DBG_DUMP(SDE_DBG_BUILT_IN_ALL, "panic");
			break;
		}

		trial++;
		autorefresh_status = hw_mdp->ops.get_autorefresh_status(hw_mdp,
					phys_enc->intf_idx);
		hw_intf->ops.check_and_reset_tearcheck(hw_intf, &tear_status);
		pr_err("enc:%d autofresh status:0x%x intf:%d tear_read:0x%x tear_write:0x%x\n",
			DRMID(phys_enc->parent), autorefresh_status, phys_enc->intf_idx - INTF_0,
			tear_status.read_count, tear_status.write_count);
		SDE_EVT32(DRMID(phys_enc->parent), phys_enc->intf_idx - INTF_0,
			autorefresh_status, tear_status.read_count,
			tear_status.write_count);
	}
}

static void _sde_encoder_phys_disable_autorefresh(struct sde_encoder_phys *phys_enc)
{
	struct sde_encoder_phys_cmd *cmd_enc = to_sde_encoder_phys_cmd(phys_enc);
	struct sde_kms *sde_kms;

	if (!phys_enc || !sde_encoder_phys_cmd_is_master(phys_enc))
		return;

	if (!sde_encoder_phys_cmd_is_autorefresh_enabled(phys_enc))
		return;

	SDE_EVT32(DRMID(phys_enc->parent), phys_enc->intf_idx - INTF_0,
			cmd_enc->autorefresh.cfg.enable);

	sde_kms = phys_enc->sde_kms;

	sde_encoder_phys_cmd_connect_te(phys_enc, false);
	_sde_encoder_phys_cmd_config_autorefresh(phys_enc, 0);
	phys_enc->autorefresh_disable_trans = true;

	if (sde_kms && sde_kms->catalog &&
			(sde_kms->catalog->autorefresh_disable_seq == AUTOREFRESH_DISABLE_SEQ1)) {
	#if defined(PXLW_IRIS_DUAL)
		if (iris_is_dual_supported())
			_iris_sde_encoder_autorefresh_disable_seq1(phys_enc);
		else
	#endif
		_sde_encoder_autorefresh_disable_seq1(phys_enc);
		_sde_encoder_autorefresh_disable_seq2(phys_enc);
	}
	sde_encoder_phys_cmd_connect_te(phys_enc, true);

	SDE_DEBUG_CMDENC(cmd_enc, "autorefresh disabled successfully\n");
}

static void sde_encoder_phys_cmd_prepare_commit(struct sde_encoder_phys *phys_enc)
{
	return _sde_encoder_phys_disable_autorefresh(phys_enc);
}

static void sde_encoder_phys_cmd_trigger_start(
		struct sde_encoder_phys *phys_enc)
{
	struct sde_encoder_phys_cmd *cmd_enc =
			to_sde_encoder_phys_cmd(phys_enc);
	u32 frame_cnt;
	struct sde_hw_pp_vsync_info info[MAX_CHANNELS_PER_ENC] = {{0}};

	if (!phys_enc)
		return;

	/* we don't issue CTL_START when using autorefresh */
	frame_cnt = _sde_encoder_phys_cmd_get_autorefresh_property(phys_enc);
	if (frame_cnt) {
		_sde_encoder_phys_cmd_config_autorefresh(phys_enc, frame_cnt);
		atomic_inc(&cmd_enc->autorefresh.kickoff_cnt);
	} else {
		sde_encoder_helper_trigger_start(phys_enc);
	}

	sde_encoder_helper_get_pp_line_count(phys_enc->parent, info);
	SDE_EVT32(DRMID(phys_enc->parent), frame_cnt, info[0].pp_idx, info[0].intf_idx,
		info[0].intf_frame_count, info[0].wr_ptr_line_count, info[0].rd_ptr_line_count,
		info[1].pp_idx, info[1].intf_idx, info[1].intf_frame_count,
		info[1].wr_ptr_line_count, info[1].rd_ptr_line_count);

	/* wr_ptr_wait_success is set true when wr_ptr arrives */
	cmd_enc->wr_ptr_wait_success = false;
}

static void _sde_encoder_phys_cmd_calculate_wd_params(struct sde_encoder_phys *phys_enc,
		struct intf_wd_jitter_params *wd_jitter)
{
	u32 nominal_te_value;
	struct sde_encoder_virt *sde_enc;
	struct msm_mode_info *mode_info;
	const u32 multiplier = 1 << 10;

	sde_enc = to_sde_encoder_virt(phys_enc->parent);
	mode_info = &sde_enc->mode_info;

	if (mode_info->wd_jitter.jitter_type & MSM_DISPLAY_WD_INSTANTANEOUS_JITTER)
		wd_jitter->jitter = mult_frac(multiplier, mode_info->wd_jitter.inst_jitter_numer,
				(mode_info->wd_jitter.inst_jitter_denom * 100));

	if (mode_info->wd_jitter.jitter_type & MSM_DISPLAY_WD_LTJ_JITTER) {
		nominal_te_value = CALCULATE_WD_LOAD_VALUE(mode_info->frame_rate) * MDP_TICK_COUNT;
		wd_jitter->ltj_max = mult_frac(nominal_te_value, mode_info->wd_jitter.ltj_max_numer,
				(mode_info->wd_jitter.ltj_max_denom) * 100);
		wd_jitter->ltj_slope = mult_frac((1 << 16), wd_jitter->ltj_max,
				(mode_info->wd_jitter.ltj_time_sec * mode_info->frame_rate));
	}

	phys_enc->hw_intf->ops.configure_wd_jitter(phys_enc->hw_intf, wd_jitter);
}

static void sde_encoder_phys_cmd_setup_vsync_source(struct sde_encoder_phys *phys_enc,
		u32 vsync_source, struct msm_display_info *disp_info)
{
	struct sde_encoder_virt *sde_enc;
	struct sde_connector *sde_conn;
	struct intf_wd_jitter_params wd_jitter = {0, 0};

	if (!phys_enc || !phys_enc->hw_intf)
		return;

	sde_enc = to_sde_encoder_virt(phys_enc->parent);
	if (!sde_enc)
		return;

	sde_conn = to_sde_connector(phys_enc->connector);

	if ((disp_info->is_te_using_watchdog_timer || sde_conn->panel_dead) &&
			phys_enc->hw_intf->ops.setup_vsync_source) {
		vsync_source = SDE_VSYNC_SOURCE_WD_TIMER_0;
		if (phys_enc->hw_intf->ops.configure_wd_jitter)
			_sde_encoder_phys_cmd_calculate_wd_params(phys_enc, &wd_jitter);
		phys_enc->hw_intf->ops.setup_vsync_source(phys_enc->hw_intf,
				sde_enc->mode_info.frame_rate);
	} else {
		sde_encoder_helper_vsync_config(phys_enc, vsync_source);
	}

	if (phys_enc->has_intf_te && phys_enc->hw_intf->ops.vsync_sel)
		phys_enc->hw_intf->ops.vsync_sel(phys_enc->hw_intf,
				vsync_source);
}

void sde_encoder_phys_cmd_add_enc_to_minidump(struct sde_encoder_phys *phys_enc)
{
	struct sde_encoder_phys_cmd *cmd_enc;
	cmd_enc =  to_sde_encoder_phys_cmd(phys_enc);

	sde_mini_dump_add_va_region("sde_enc_phys_cmd", sizeof(*cmd_enc), cmd_enc);
}

static void sde_encoder_phys_cmd_init_ops(struct sde_encoder_phys_ops *ops)
{
	ops->prepare_commit = sde_encoder_phys_cmd_prepare_commit;
	ops->is_master = sde_encoder_phys_cmd_is_master;
	ops->mode_set = sde_encoder_phys_cmd_mode_set;
	ops->cont_splash_mode_set = sde_encoder_phys_cmd_cont_splash_mode_set;
	ops->mode_fixup = sde_encoder_phys_cmd_mode_fixup;
	ops->enable = sde_encoder_phys_cmd_enable;
	ops->disable = sde_encoder_phys_cmd_disable;
	ops->destroy = sde_encoder_phys_cmd_destroy;
	ops->get_hw_resources = sde_encoder_phys_cmd_get_hw_resources;
	ops->control_vblank_irq = sde_encoder_phys_cmd_control_vblank_irq;
	ops->wait_for_commit_done = sde_encoder_phys_cmd_wait_for_commit_done;
	ops->prepare_for_kickoff = sde_encoder_phys_cmd_prepare_for_kickoff;
	ops->wait_for_tx_complete = sde_encoder_phys_cmd_wait_for_tx_complete;
	ops->wait_for_vblank = sde_encoder_phys_cmd_wait_for_vblank;
	ops->trigger_flush = sde_encoder_helper_trigger_flush;
	ops->trigger_start = sde_encoder_phys_cmd_trigger_start;
	ops->needs_single_flush = sde_encoder_phys_needs_single_flush;
	ops->hw_reset = sde_encoder_helper_hw_reset;
	ops->irq_control = sde_encoder_phys_cmd_irq_control;
	ops->update_split_role = sde_encoder_phys_cmd_update_split_role;
	ops->restore = sde_encoder_phys_cmd_enable_helper;
	ops->control_te = sde_encoder_phys_cmd_connect_te;
	ops->is_autorefresh_enabled =
			sde_encoder_phys_cmd_is_autorefresh_enabled;
	ops->get_line_count = sde_encoder_phys_cmd_te_get_line_count;
	ops->wait_for_active = NULL;
	ops->setup_vsync_source = sde_encoder_phys_cmd_setup_vsync_source;
	ops->setup_misr = sde_encoder_helper_setup_misr;
	ops->collect_misr = sde_encoder_helper_collect_misr;
	ops->add_to_minidump = sde_encoder_phys_cmd_add_enc_to_minidump;
	ops->disable_autorefresh = _sde_encoder_phys_disable_autorefresh;
	ops->wait_for_vsync_on_autorefresh_busy =
			_sde_encoder_phys_wait_for_vsync_on_autorefresh_busy;
}

static inline bool sde_encoder_phys_cmd_intf_te_supported(
		const struct sde_mdss_cfg *sde_cfg, enum sde_intf idx)
{
	if (sde_cfg && ((idx - INTF_0) < sde_cfg->intf_count))
		return test_bit(SDE_INTF_TE,
				&(sde_cfg->intf[idx - INTF_0].features));
	return false;
}

struct sde_encoder_phys *sde_encoder_phys_cmd_init(
		struct sde_enc_phys_init_params *p)
{
	struct sde_encoder_phys *phys_enc = NULL;
	struct sde_encoder_phys_cmd *cmd_enc = NULL;
	struct sde_hw_mdp *hw_mdp;
	struct sde_encoder_irq *irq;
	int i, ret = 0;

	SDE_DEBUG("intf %d\n", p->intf_idx - INTF_0);

	cmd_enc = kzalloc(sizeof(*cmd_enc), GFP_KERNEL);
	if (!cmd_enc) {
		ret = -ENOMEM;
		SDE_ERROR("failed to allocate\n");
		goto fail;
	}
	phys_enc = &cmd_enc->base;

	hw_mdp = sde_rm_get_mdp(&p->sde_kms->rm);
	if (IS_ERR_OR_NULL(hw_mdp)) {
		ret = PTR_ERR(hw_mdp);
		SDE_ERROR("failed to get mdptop\n");
		goto fail_mdp_init;
	}
	phys_enc->hw_mdptop = hw_mdp;
	phys_enc->intf_idx = p->intf_idx;

	phys_enc->parent = p->parent;
	phys_enc->parent_ops = p->parent_ops;
	phys_enc->sde_kms = p->sde_kms;
	phys_enc->split_role = p->split_role;
	phys_enc->intf_mode = INTF_MODE_CMD;
	phys_enc->enc_spinlock = p->enc_spinlock;
	phys_enc->vblank_ctl_lock = p->vblank_ctl_lock;
	cmd_enc->stream_sel = 0;
	phys_enc->enable_state = SDE_ENC_DISABLED;
	phys_enc->kickoff_timeout_ms = DEFAULT_KICKOFF_TIMEOUT_MS;
	sde_encoder_phys_cmd_init_ops(&phys_enc->ops);
	phys_enc->comp_type = p->comp_type;

	phys_enc->has_intf_te = sde_encoder_phys_cmd_intf_te_supported(
			phys_enc->sde_kms->catalog, phys_enc->intf_idx);

	for (i = 0; i < INTR_IDX_MAX; i++) {
		irq = &phys_enc->irq[i];
		INIT_LIST_HEAD(&irq->cb.list);
		irq->irq_idx = -EINVAL;
		irq->hw_idx = -EINVAL;
		irq->cb.arg = phys_enc;
	}

	irq = &phys_enc->irq[INTR_IDX_CTL_START];
	irq->name = "ctl_start";
	irq->intr_type = SDE_IRQ_TYPE_CTL_START;
	irq->intr_idx = INTR_IDX_CTL_START;
	irq->cb.func = NULL;

	irq = &phys_enc->irq[INTR_IDX_CTL_DONE];
	irq->name = "ctl_done";
	irq->intr_type = SDE_IRQ_TYPE_CTL_DONE;
	irq->intr_idx = INTR_IDX_CTL_DONE;
	irq->cb.func = sde_encoder_phys_cmd_ctl_done_irq;

	irq = &phys_enc->irq[INTR_IDX_PINGPONG];
	irq->name = "pp_done";
	irq->intr_type = SDE_IRQ_TYPE_PING_PONG_COMP;
	irq->intr_idx = INTR_IDX_PINGPONG;
	irq->cb.func = sde_encoder_phys_cmd_pp_tx_done_irq;

	irq = &phys_enc->irq[INTR_IDX_RDPTR];
	irq->intr_idx = INTR_IDX_RDPTR;
	irq->name = "te_rd_ptr";

	if (phys_enc->has_intf_te)
		irq->intr_type = SDE_IRQ_TYPE_INTF_TEAR_RD_PTR;
	else
		irq->intr_type = SDE_IRQ_TYPE_PING_PONG_RD_PTR;

	irq->cb.func = sde_encoder_phys_cmd_te_rd_ptr_irq;

	irq = &phys_enc->irq[INTR_IDX_AUTOREFRESH_DONE];
	irq->name = "autorefresh_done";

	if (phys_enc->has_intf_te)
		irq->intr_type = SDE_IRQ_TYPE_INTF_TEAR_AUTO_REF;
	else
		irq->intr_type = SDE_IRQ_TYPE_PING_PONG_AUTO_REF;

	irq->intr_idx = INTR_IDX_AUTOREFRESH_DONE;
	irq->cb.func = sde_encoder_phys_cmd_autorefresh_done_irq;

	irq = &phys_enc->irq[INTR_IDX_WRPTR];
	irq->intr_idx = INTR_IDX_WRPTR;
	irq->name = "wr_ptr";

	if (phys_enc->has_intf_te)
		irq->intr_type = SDE_IRQ_TYPE_INTF_TEAR_WR_PTR;
	else
		irq->intr_type = SDE_IRQ_TYPE_PING_PONG_WR_PTR;
	irq->cb.func = sde_encoder_phys_cmd_wr_ptr_irq;

	atomic_set(&phys_enc->vblank_refcount, 0);
	atomic_set(&phys_enc->pending_kickoff_cnt, 0);
	atomic_set(&phys_enc->pending_retire_fence_cnt, 0);
	atomic_set(&cmd_enc->pending_vblank_cnt, 0);
	init_waitqueue_head(&phys_enc->pending_kickoff_wq);
	init_waitqueue_head(&cmd_enc->pending_vblank_wq);
	atomic_set(&cmd_enc->autorefresh.kickoff_cnt, 0);
	init_waitqueue_head(&cmd_enc->autorefresh.kickoff_wq);
	INIT_LIST_HEAD(&cmd_enc->te_timestamp_list);
	for (i = 0; i < MAX_TE_PROFILE_COUNT; i++)
		list_add(&cmd_enc->te_timestamp[i].list,
				&cmd_enc->te_timestamp_list);

	SDE_DEBUG_CMDENC(cmd_enc, "created\n");

	return phys_enc;

fail_mdp_init:
	kfree(cmd_enc);
fail:
	return ERR_PTR(ret);
}
