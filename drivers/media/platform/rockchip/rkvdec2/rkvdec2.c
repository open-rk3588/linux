// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip Video Decoder 2 driver
 *
 * Copyright (C) 2024 Collabora, Ltd.
 *  Detlev Casanova <detlev.casanova@collabora.com>
 *
 * Based on rkvdec driver by Boris Brezillon <boris.brezillon@collabora.com>
 */

#include <linux/clk.h>
#include <linux/genalloc.h>
#include <linux/interrupt.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/videodev2.h>
#include <linux/workqueue.h>
#include <media/v4l2-event.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-vmalloc.h>

#include "rkvdec2.h"

static inline bool rkvdec2_image_fmt_match(enum rkvdec2_image_fmt fmt1,
					   enum rkvdec2_image_fmt fmt2)
{
	return fmt1 == fmt2 || fmt2 == RKVDEC2_IMG_FMT_ANY ||
	       fmt1 == RKVDEC2_IMG_FMT_ANY;
}

static u32 rkvdec2_enum_decoded_fmt(struct rkvdec2_ctx *ctx, int index,
				    enum rkvdec2_image_fmt image_fmt)
{
	const struct rkvdec2_coded_fmt_desc *desc = ctx->coded_fmt_desc;
	int fmt_idx = -1;
	unsigned int i;

	if (WARN_ON(!desc))
		return 0;

	for (i = 0; i < desc->num_decoded_fmts; i++) {
		if (!rkvdec2_image_fmt_match(desc->decoded_fmts[i].image_fmt,
					   image_fmt))
			continue;
		fmt_idx++;
		if (index == fmt_idx)
			return desc->decoded_fmts[i].fourcc;
	}

	return 0;
}

static bool rkvdec2_is_valid_fmt(struct rkvdec2_ctx *ctx, u32 fourcc,
				 enum rkvdec2_image_fmt image_fmt)
{
	const struct rkvdec2_coded_fmt_desc *desc = ctx->coded_fmt_desc;
	unsigned int i;

	for (i = 0; i < desc->num_decoded_fmts; i++) {
		if (rkvdec2_image_fmt_match(desc->decoded_fmts[i].image_fmt,
					   image_fmt) &&
		    desc->decoded_fmts[i].fourcc == fourcc)
			return true;
	}

	return false;
}

static u32 rkvdec2_fill_decoded_pixfmt(struct rkvdec2_ctx *ctx,
				       struct v4l2_pix_format_mplane *pix_mp)
{
	u32 colmv_offset;

	v4l2_fill_pixfmt_mp(pix_mp, pix_mp->pixelformat,
			    pix_mp->width, pix_mp->height);

	colmv_offset = pix_mp->plane_fmt[0].sizeimage;

	pix_mp->plane_fmt[0].sizeimage += 128 *
		DIV_ROUND_UP(pix_mp->width, 16) *
		DIV_ROUND_UP(pix_mp->height, 16);

	return colmv_offset;
}

static void rkvdec2_reset_fmt(struct rkvdec2_ctx *ctx, struct v4l2_format *f,
			      u32 fourcc)
{
	memset(f, 0, sizeof(*f));
	f->fmt.pix_mp.pixelformat = fourcc;
	f->fmt.pix_mp.field = V4L2_FIELD_NONE;
	f->fmt.pix_mp.colorspace = V4L2_COLORSPACE_REC709;
	f->fmt.pix_mp.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	f->fmt.pix_mp.quantization = V4L2_QUANTIZATION_DEFAULT;
	f->fmt.pix_mp.xfer_func = V4L2_XFER_FUNC_DEFAULT;
}

static void rkvdec2_reset_decoded_fmt(struct rkvdec2_ctx *ctx)
{
	struct v4l2_format *f = &ctx->decoded_fmt;
	u32 fourcc;

	fourcc = rkvdec2_enum_decoded_fmt(ctx, 0, ctx->image_fmt);
	rkvdec2_reset_fmt(ctx, f, fourcc);
	f->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	f->fmt.pix_mp.width = ctx->coded_fmt.fmt.pix_mp.width;
	f->fmt.pix_mp.height = ctx->coded_fmt.fmt.pix_mp.height;
	ctx->colmv_offset = rkvdec2_fill_decoded_pixfmt(ctx, &f->fmt.pix_mp);
}

static int rkvdec2_try_ctrl(struct v4l2_ctrl *ctrl)
{
	struct rkvdec2_ctx *ctx = container_of(ctrl->handler, struct rkvdec2_ctx, ctrl_hdl);
	const struct rkvdec2_coded_fmt_desc *desc = ctx->coded_fmt_desc;
	struct v4l2_pix_format_mplane *pix_mp = &ctx->decoded_fmt.fmt.pix_mp;
	enum rkvdec2_image_fmt image_fmt;
	struct vb2_queue *vq;
	int ret;

	if (desc->ops->try_ctrl) {
		ret = desc->ops->try_ctrl(ctx, ctrl);
		if (ret)
			return ret;
	}

	if (!desc->ops->get_image_fmt)
		return 0;

	image_fmt = desc->ops->get_image_fmt(ctx, ctrl);
	if (ctx->image_fmt == image_fmt)
		return 0;

	if (rkvdec2_is_valid_fmt(ctx, pix_mp->pixelformat, image_fmt))
		return 0;

	/* format change not allowed when queue is busy */
	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx,
			     V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	if (vb2_is_busy(vq)) {
		dev_err(ctx->dev->dev, "Queue is busy\n");
		return -EINVAL;
	}
	return 0;
}

static int rkvdec2_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct rkvdec2_ctx *ctx = container_of(ctrl->handler, struct rkvdec2_ctx, ctrl_hdl);
	const struct rkvdec2_coded_fmt_desc *desc = ctx->coded_fmt_desc;
	struct v4l2_pix_format_mplane *pix_mp = &ctx->decoded_fmt.fmt.pix_mp;
	enum rkvdec2_image_fmt image_fmt;

	if (!desc->ops->get_image_fmt)
		return 0;

	image_fmt = desc->ops->get_image_fmt(ctx, ctrl);
	if (ctx->image_fmt == image_fmt)
		return 0;

	ctx->image_fmt = image_fmt;
	if (!rkvdec2_is_valid_fmt(ctx, pix_mp->pixelformat, ctx->image_fmt))
		rkvdec2_reset_decoded_fmt(ctx);

	return 0;
}

static const struct v4l2_ctrl_ops rkvdec2_ctrl_ops = {
	.try_ctrl = rkvdec2_try_ctrl,
	.s_ctrl = rkvdec2_s_ctrl,
};

static const struct rkvdec2_ctrl_desc rkvdec2_h264_ctrl_descs[] = {
	{
		.cfg.id = V4L2_CID_STATELESS_H264_DECODE_PARAMS,
	},
	{
		.cfg.id = V4L2_CID_STATELESS_H264_SPS,
		.cfg.ops = &rkvdec2_ctrl_ops,
	},
	{
		.cfg.id = V4L2_CID_STATELESS_H264_PPS,
	},
	{
		.cfg.id = V4L2_CID_STATELESS_H264_SCALING_MATRIX,
	},
	{
		.cfg.id = V4L2_CID_STATELESS_H264_DECODE_MODE,
		.cfg.min = V4L2_STATELESS_H264_DECODE_MODE_FRAME_BASED,
		.cfg.max = V4L2_STATELESS_H264_DECODE_MODE_FRAME_BASED,
		.cfg.def = V4L2_STATELESS_H264_DECODE_MODE_FRAME_BASED,
	},
	{
		.cfg.id = V4L2_CID_STATELESS_H264_START_CODE,
		.cfg.min = V4L2_STATELESS_H264_START_CODE_ANNEX_B,
		.cfg.def = V4L2_STATELESS_H264_START_CODE_ANNEX_B,
		.cfg.max = V4L2_STATELESS_H264_START_CODE_ANNEX_B,
	},
	{
		.cfg.id = V4L2_CID_MPEG_VIDEO_H264_PROFILE,
		.cfg.min = V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE,
		.cfg.max = V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_422_INTRA,
		.cfg.menu_skip_mask =
			BIT(V4L2_MPEG_VIDEO_H264_PROFILE_EXTENDED) |
			BIT(V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_444_PREDICTIVE),
		.cfg.def = V4L2_MPEG_VIDEO_H264_PROFILE_MAIN,
	},
	{
		.cfg.id = V4L2_CID_MPEG_VIDEO_H264_LEVEL,
		.cfg.min = V4L2_MPEG_VIDEO_H264_LEVEL_1_0,
		.cfg.max = V4L2_MPEG_VIDEO_H264_LEVEL_6_1,
	},
};

static const struct rkvdec2_ctrls rkvdec2_h264_ctrls = {
	.ctrls = rkvdec2_h264_ctrl_descs,
	.num_ctrls = ARRAY_SIZE(rkvdec2_h264_ctrl_descs),
};

static const struct rkvdec2_decoded_fmt_desc rkvdec2_h264_decoded_fmts[] = {
	{
		.fourcc = V4L2_PIX_FMT_NV12,
		.image_fmt = RKVDEC2_IMG_FMT_420_8BIT,
	},
	{
		.fourcc = V4L2_PIX_FMT_NV15,
		.image_fmt = RKVDEC2_IMG_FMT_420_10BIT,
	},
	{
		.fourcc = V4L2_PIX_FMT_NV16,
		.image_fmt = RKVDEC2_IMG_FMT_422_8BIT,
	},
	{
		.fourcc = V4L2_PIX_FMT_NV20,
		.image_fmt = RKVDEC2_IMG_FMT_422_10BIT,
	},
};

static const struct rkvdec2_coded_fmt_desc rkvdec2_coded_fmts[] = {
	{
		.fourcc = V4L2_PIX_FMT_H264_SLICE,
		.frmsize = {
			.min_width = 64,
			.max_width =  65520,
			.step_width = 64,
			.min_height = 16,
			.max_height =  65520,
			.step_height = 16,
		},
		.ctrls = &rkvdec2_h264_ctrls,
		.ops = &rkvdec2_h264_fmt_ops,
		.num_decoded_fmts = ARRAY_SIZE(rkvdec2_h264_decoded_fmts),
		.decoded_fmts = rkvdec2_h264_decoded_fmts,
		.subsystem_flags = VB2_V4L2_FL_SUPPORTS_M2M_HOLD_CAPTURE_BUF,
	},
};

static const struct rkvdec2_coded_fmt_desc *rkvdec2_find_coded_fmt_desc(struct rkvdec2_ctx *ctx,
									u32 fourcc)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(rkvdec2_coded_fmts); i++) {
		if (rkvdec2_coded_fmts[i].fourcc == fourcc)
			return &rkvdec2_coded_fmts[i];
	}

	return NULL;
}

static void rkvdec2_reset_coded_fmt(struct rkvdec2_ctx *ctx)
{
	struct v4l2_format *f = &ctx->coded_fmt;

	ctx->coded_fmt_desc = &rkvdec2_coded_fmts[0];
	rkvdec2_reset_fmt(ctx, f, ctx->coded_fmt_desc->fourcc);

	f->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	f->fmt.pix_mp.width = ctx->coded_fmt_desc->frmsize.min_width;
	f->fmt.pix_mp.height = ctx->coded_fmt_desc->frmsize.min_height;

	if (ctx->coded_fmt_desc->ops->adjust_fmt)
		ctx->coded_fmt_desc->ops->adjust_fmt(ctx, f);
}

static int rkvdec2_enum_framesizes(struct file *file, void *priv,
				   struct v4l2_frmsizeenum *fsize)
{
	struct rkvdec2_ctx *ctx = fh_to_rkvdec2_ctx(priv);
	const struct rkvdec2_coded_fmt_desc *desc;

	if (fsize->index != 0)
		return -EINVAL;

	desc = rkvdec2_find_coded_fmt_desc(ctx, fsize->pixel_format);
	if (!desc)
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_CONTINUOUS;

	fsize->stepwise.min_height = 1;
	fsize->stepwise.min_width = 1;
	fsize->stepwise.step_height = 1;
	fsize->stepwise.step_width = 1;
	fsize->stepwise.max_height = desc->frmsize.max_height;
	fsize->stepwise.max_width = desc->frmsize.max_width;

	return 0;
}

static int rkvdec2_querycap(struct file *file, void *priv,
			    struct v4l2_capability *cap)
{
	struct rkvdec2_dev *rkvdec = video_drvdata(file);
	struct video_device *vdev = video_devdata(file);

	strscpy(cap->driver, rkvdec->dev->driver->name,
		sizeof(cap->driver));
	strscpy(cap->card, vdev->name, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s",
		 rkvdec->dev->driver->name);
	return 0;
}

static int rkvdec2_try_capture_fmt(struct file *file, void *priv,
				   struct v4l2_format *f)
{
	struct rkvdec2_ctx *ctx = fh_to_rkvdec2_ctx(priv);
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	const struct rkvdec2_coded_fmt_desc *coded_desc;

	/*
	 * The codec context should point to a coded format desc, if the format
	 * on the coded end has not been set yet, it should point to the
	 * default value.
	 */
	coded_desc = ctx->coded_fmt_desc;
	if (WARN_ON(!coded_desc))
		return -EINVAL;

	if (!rkvdec2_is_valid_fmt(ctx, pix_mp->pixelformat, ctx->image_fmt))
		pix_mp->pixelformat = rkvdec2_enum_decoded_fmt(ctx, 0,
							      ctx->image_fmt);

	/* Always apply the frmsize constraint of the coded end. */
	pix_mp->width = max(pix_mp->width, ctx->coded_fmt.fmt.pix_mp.width);
	pix_mp->height = max(pix_mp->height, ctx->coded_fmt.fmt.pix_mp.height);
	v4l2_apply_frmsize_constraints(&pix_mp->width,
				       &pix_mp->height,
				       &coded_desc->frmsize);

	rkvdec2_fill_decoded_pixfmt(ctx, pix_mp);

	pix_mp->field = V4L2_FIELD_NONE;

	return 0;
}

static int rkvdec2_try_output_fmt(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	struct rkvdec2_ctx *ctx = fh_to_rkvdec2_ctx(priv);
	const struct rkvdec2_coded_fmt_desc *desc;

	desc = rkvdec2_find_coded_fmt_desc(ctx, pix_mp->pixelformat);
	if (!desc) {
		pix_mp->pixelformat = rkvdec2_coded_fmts[0].fourcc;
		desc = &rkvdec2_coded_fmts[0];
	}

	v4l2_apply_frmsize_constraints(&pix_mp->width,
				       &pix_mp->height,
				       &desc->frmsize);

	pix_mp->field = V4L2_FIELD_NONE;
	/* All coded formats are considered single planar for now. */
	pix_mp->num_planes = 1;

	if (desc->ops->adjust_fmt) {
		int ret;

		ret = desc->ops->adjust_fmt(ctx, f);
		if (ret)
			return ret;
	}

	return 0;
}

static int rkvdec2_s_capture_fmt(struct file *file, void *priv,
				 struct v4l2_format *f)
{
	struct rkvdec2_ctx *ctx = fh_to_rkvdec2_ctx(priv);
	struct vb2_queue *vq;
	int ret;

	/* Change not allowed if queue is busy */
	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx,
			     V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	if (vb2_is_busy(vq))
		return -EBUSY;

	ret = rkvdec2_try_capture_fmt(file, priv, f);
	if (ret)
		return ret;

	ctx->decoded_fmt = *f;
	return 0;
}

static int rkvdec2_s_output_fmt(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct rkvdec2_ctx *ctx = fh_to_rkvdec2_ctx(priv);
	struct v4l2_m2m_ctx *m2m_ctx = ctx->fh.m2m_ctx;
	const struct rkvdec2_coded_fmt_desc *desc;
	struct v4l2_format *cap_fmt;
	struct vb2_queue *peer_vq, *vq;
	int ret;

	/*
	 * In order to support dynamic resolution change, the decoder admits
	 * a resolution change, as long as the pixelformat remains. Can't be
	 * done if streaming.
	 */
	vq = v4l2_m2m_get_vq(m2m_ctx, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
	if (vb2_is_streaming(vq) ||
	    (vb2_is_busy(vq) &&
	     f->fmt.pix_mp.pixelformat != ctx->coded_fmt.fmt.pix_mp.pixelformat))
		return -EBUSY;

	/*
	 * Since format change on the OUTPUT queue will reset the CAPTURE
	 * queue, we can't allow doing so when the CAPTURE queue has buffers
	 * allocated.
	 */
	peer_vq = v4l2_m2m_get_vq(m2m_ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	if (vb2_is_busy(peer_vq))
		return -EBUSY;

	ret = rkvdec2_try_output_fmt(file, priv, f);
	if (ret)
		return ret;

	desc = rkvdec2_find_coded_fmt_desc(ctx, f->fmt.pix_mp.pixelformat);
	if (!desc)
		return -EINVAL;

	ctx->coded_fmt_desc = desc;
	ctx->coded_fmt = *f;

	/*
	 * Current decoded format might have become invalid with newly
	 * selected codec, so reset it to default just to be safe and
	 * keep internal driver state sane. User is mandated to set
	 * the decoded format again after we return, so we don't need
	 * anything smarter.
	 *
	 * Note that this will propagate any size changes to the decoded format.
	 */
	rkvdec2_reset_decoded_fmt(ctx);

	/* Propagate colorspace information to capture. */
	cap_fmt = &ctx->decoded_fmt;
	cap_fmt->fmt.pix_mp.colorspace = f->fmt.pix_mp.colorspace;
	cap_fmt->fmt.pix_mp.xfer_func = f->fmt.pix_mp.xfer_func;
	cap_fmt->fmt.pix_mp.ycbcr_enc = f->fmt.pix_mp.ycbcr_enc;
	cap_fmt->fmt.pix_mp.quantization = f->fmt.pix_mp.quantization;

	/* Enable format specific queue features */
	vq->subsystem_flags |= desc->subsystem_flags;

	return 0;
}

static int rkvdec2_g_output_fmt(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct rkvdec2_ctx *ctx = fh_to_rkvdec2_ctx(priv);

	*f = ctx->coded_fmt;
	return 0;
}

static int rkvdec2_g_capture_fmt(struct file *file, void *priv,
				 struct v4l2_format *f)
{
	struct rkvdec2_ctx *ctx = fh_to_rkvdec2_ctx(priv);

	*f = ctx->decoded_fmt;
	return 0;
}

static int rkvdec2_enum_output_fmt(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	if (f->index >= ARRAY_SIZE(rkvdec2_coded_fmts))
		return -EINVAL;

	f->pixelformat = rkvdec2_coded_fmts[f->index].fourcc;

	return 0;
}

static int rkvdec2_enum_capture_fmt(struct file *file, void *priv,
				    struct v4l2_fmtdesc *f)
{
	struct rkvdec2_ctx *ctx = fh_to_rkvdec2_ctx(priv);
	u32 fourcc;

	fourcc = rkvdec2_enum_decoded_fmt(ctx, f->index, ctx->image_fmt);
	if (!fourcc)
		return -EINVAL;

	f->pixelformat = fourcc;

	return 0;
}

static const struct v4l2_ioctl_ops rkvdec2_ioctl_ops = {
	.vidioc_querycap = rkvdec2_querycap,
	.vidioc_enum_framesizes = rkvdec2_enum_framesizes,

	.vidioc_try_fmt_vid_cap_mplane = rkvdec2_try_capture_fmt,
	.vidioc_try_fmt_vid_out_mplane = rkvdec2_try_output_fmt,
	.vidioc_s_fmt_vid_out_mplane = rkvdec2_s_output_fmt,
	.vidioc_s_fmt_vid_cap_mplane = rkvdec2_s_capture_fmt,
	.vidioc_g_fmt_vid_out_mplane = rkvdec2_g_output_fmt,
	.vidioc_g_fmt_vid_cap_mplane = rkvdec2_g_capture_fmt,
	.vidioc_enum_fmt_vid_out = rkvdec2_enum_output_fmt,
	.vidioc_enum_fmt_vid_cap = rkvdec2_enum_capture_fmt,

	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,

	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,

	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,

	.vidioc_decoder_cmd = v4l2_m2m_ioctl_stateless_decoder_cmd,
	.vidioc_try_decoder_cmd = v4l2_m2m_ioctl_stateless_try_decoder_cmd,
};

static int rkvdec2_queue_setup(struct vb2_queue *vq, unsigned int *num_buffers,
			       unsigned int *num_planes, unsigned int sizes[],
			       struct device *alloc_devs[])
{
	struct rkvdec2_ctx *ctx = vb2_get_drv_priv(vq);
	struct v4l2_format *f;
	unsigned int i;

	if (V4L2_TYPE_IS_OUTPUT(vq->type))
		f = &ctx->coded_fmt;
	else
		f = &ctx->decoded_fmt;

	if (*num_planes) {
		if (*num_planes != f->fmt.pix_mp.num_planes)
			return -EINVAL;

		for (i = 0; i < f->fmt.pix_mp.num_planes; i++)
			if (sizes[i] < f->fmt.pix_mp.plane_fmt[i].sizeimage)
				return -EINVAL;
	} else {
		*num_planes = f->fmt.pix_mp.num_planes;
		for (i = 0; i < f->fmt.pix_mp.num_planes; i++)
			sizes[i] = f->fmt.pix_mp.plane_fmt[i].sizeimage;
	}

	return 0;
}

static int rkvdec2_buf_prepare(struct vb2_buffer *vb)
{
	struct vb2_queue *vq = vb->vb2_queue;
	struct rkvdec2_ctx *ctx = vb2_get_drv_priv(vq);
	struct v4l2_format *f;
	unsigned int i;

	if (V4L2_TYPE_IS_OUTPUT(vq->type))
		f = &ctx->coded_fmt;
	else
		f = &ctx->decoded_fmt;

	for (i = 0; i < f->fmt.pix_mp.num_planes; ++i) {
		u32 sizeimage = f->fmt.pix_mp.plane_fmt[i].sizeimage;

		if (vb2_plane_size(vb, i) < sizeimage)
			return -EINVAL;
	}

	/*
	 * Buffer's bytesused must be written by driver for CAPTURE buffers.
	 * (for OUTPUT buffers, if userspace passes 0 bytesused, v4l2-core sets
	 * it to buffer length).
	 */
	if (V4L2_TYPE_IS_CAPTURE(vq->type))
		vb2_set_plane_payload(vb, 0, f->fmt.pix_mp.plane_fmt[0].sizeimage);

	return 0;
}

static void rkvdec2_buf_queue(struct vb2_buffer *vb)
{
	struct rkvdec2_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vbuf);
}

static int rkvdec2_buf_out_validate(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);

	vbuf->field = V4L2_FIELD_NONE;
	return 0;
}

static void rkvdec2_buf_request_complete(struct vb2_buffer *vb)
{
	struct rkvdec2_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_ctrl_request_complete(vb->req_obj.req, &ctx->ctrl_hdl);
}

enum rcb_axis {
	PIC_WIDTH = 0,
	PIC_HEIGHT = 1
};

struct rcb_size_info {
	u8 multiplier;
	enum rcb_axis axis;
};

static struct rcb_size_info rcb_sizes[] = {
	{6,	PIC_WIDTH},	// intrar
	{1,	PIC_WIDTH},	// transdr (Is actually 0.4*pic_width)
	{1,	PIC_HEIGHT},	// transdc (Is actually 0.1*pic_height)
	{3,	PIC_WIDTH},	// streamdr
	{6,	PIC_WIDTH},	// interr
	{3,	PIC_HEIGHT},	// interc
	{22,	PIC_WIDTH},	// dblkr
	{6,	PIC_WIDTH},	// saor
	{11,	PIC_WIDTH},	// fbcr
	{67,	PIC_HEIGHT},	// filtc col
};

#define RCB_SIZE(n, w, h) (rcb_sizes[(n)].multiplier * (rcb_sizes[(n)].axis ? (h) : (w)))

static void rkvdec2_free_rcb(struct rkvdec2_ctx *ctx)
{
	struct rkvdec2_dev *rkvdec = ctx->dev;
	u32 width, height;
	unsigned long virt_addr;
	int i;

	width = ctx->decoded_fmt.fmt.pix_mp.width;
	height = ctx->decoded_fmt.fmt.pix_mp.height;

	for (i = 0; i < RKVDEC2_RCB_COUNT; i++) {
		size_t rcb_size = ctx->rcb_bufs[i].size;

		if (!ctx->rcb_bufs[i].cpu)
			continue;

		switch (ctx->rcb_bufs[i].type) {
		case RKVDEC2_ALLOC_SRAM:
			virt_addr = (unsigned long)ctx->rcb_bufs[i].cpu;

			iommu_unmap(rkvdec->iommu_domain, virt_addr, rcb_size);
			gen_pool_free(ctx->dev->sram_pool, virt_addr, rcb_size);
			break;
		case RKVDEC2_ALLOC_DMA:
			dma_free_coherent(ctx->dev->dev,
					  rcb_size,
					  ctx->rcb_bufs[i].cpu,
					  ctx->rcb_bufs[i].dma);
			break;
		}
	}
}

static int rkvdec2_allocate_rcb(struct rkvdec2_ctx *ctx)
{
	int ret, i;
	u32 width, height;
	struct rkvdec2_dev *rkvdec = ctx->dev;

	memset(ctx->rcb_bufs, 0, sizeof(*ctx->rcb_bufs));

	width = ctx->decoded_fmt.fmt.pix_mp.width;
	height = ctx->decoded_fmt.fmt.pix_mp.height;

	for (i = 0; i < RKVDEC2_RCB_COUNT; i++) {
		void *cpu = NULL;
		dma_addr_t dma;
		size_t rcb_size = RCB_SIZE(i, width, height);
		enum rkvdec2_alloc_type alloc_type = RKVDEC2_ALLOC_SRAM;

		/* Try allocating an SRAM buffer */
		if (ctx->dev->sram_pool) {
			if (rkvdec->iommu_domain)
				rcb_size = ALIGN(rcb_size, 0x1000);

			cpu = gen_pool_dma_zalloc_align(ctx->dev->sram_pool,
						rcb_size,
						&dma,
						0x1000);
		}

		/* If an IOMMU is used, map the SRAM address through it */
		if (cpu && rkvdec->iommu_domain) {
			unsigned long virt_addr = (unsigned long)cpu;
			phys_addr_t phys_addr = dma;

			ret = iommu_map(rkvdec->iommu_domain, virt_addr, phys_addr,
					rcb_size, IOMMU_READ | IOMMU_WRITE, 0);
			if (ret) {
				gen_pool_free(ctx->dev->sram_pool,
				      (unsigned long)cpu,
				      rcb_size);
				cpu = NULL;
				goto ram_fallback;
			}

			/*
			 * The registers will be configured with the virtual
			 * address so that it goes through the IOMMU
			 */
			dma = virt_addr;
		}

ram_fallback:
		/* Fallback to RAM */
		if (!cpu) {
			rcb_size = RCB_SIZE(i, width, height);
			cpu = dma_alloc_coherent(ctx->dev->dev,
						 rcb_size,
						 &dma,
						 GFP_KERNEL);
			alloc_type = RKVDEC2_ALLOC_DMA;
		}

		if (!cpu) {
			ret = -ENOMEM;
			goto err_alloc;
		}

		ctx->rcb_bufs[i].cpu = cpu;
		ctx->rcb_bufs[i].dma = dma;
		ctx->rcb_bufs[i].size = rcb_size;
		ctx->rcb_bufs[i].type = alloc_type;
	}

	return 0;

err_alloc:
	rkvdec2_free_rcb(ctx);

	return ret;
}

static int rkvdec2_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct rkvdec2_ctx *ctx = vb2_get_drv_priv(q);
	const struct rkvdec2_coded_fmt_desc *desc;
	int ret;

	if (V4L2_TYPE_IS_CAPTURE(q->type))
		return 0;

	desc = ctx->coded_fmt_desc;
	if (WARN_ON(!desc))
		return -EINVAL;

	ret = rkvdec2_allocate_rcb(ctx);
	if (ret)
		return ret;

	if (desc->ops->start) {
		ret = desc->ops->start(ctx);
		if (ret)
			goto err_ops_start;
	}

	return 0;

err_ops_start:
	rkvdec2_free_rcb(ctx);

	return ret;
}

static void rkvdec2_queue_cleanup(struct vb2_queue *vq, u32 state)
{
	struct rkvdec2_ctx *ctx = vb2_get_drv_priv(vq);

	while (true) {
		struct vb2_v4l2_buffer *vbuf;

		if (V4L2_TYPE_IS_OUTPUT(vq->type))
			vbuf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
		else
			vbuf = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);

		if (!vbuf)
			break;

		v4l2_ctrl_request_complete(vbuf->vb2_buf.req_obj.req,
					   &ctx->ctrl_hdl);
		v4l2_m2m_buf_done(vbuf, state);
	}
}

static void rkvdec2_stop_streaming(struct vb2_queue *q)
{
	struct rkvdec2_ctx *ctx = vb2_get_drv_priv(q);

	if (V4L2_TYPE_IS_OUTPUT(q->type)) {
		const struct rkvdec2_coded_fmt_desc *desc = ctx->coded_fmt_desc;

		if (WARN_ON(!desc))
			return;

		if (desc->ops->stop)
			desc->ops->stop(ctx);

		rkvdec2_free_rcb(ctx);
	}

	rkvdec2_queue_cleanup(q, VB2_BUF_STATE_ERROR);
}

static const struct vb2_ops rkvdec2_queue_ops = {
	.queue_setup = rkvdec2_queue_setup,
	.buf_prepare = rkvdec2_buf_prepare,
	.buf_queue = rkvdec2_buf_queue,
	.buf_out_validate = rkvdec2_buf_out_validate,
	.buf_request_complete = rkvdec2_buf_request_complete,
	.start_streaming = rkvdec2_start_streaming,
	.stop_streaming = rkvdec2_stop_streaming,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
};

static int rkvdec2_request_validate(struct media_request *req)
{
	unsigned int count;

	count = vb2_request_buffer_cnt(req);
	if (!count)
		return -ENOENT;
	else if (count > 1)
		return -EINVAL;

	return vb2_request_validate(req);
}

static const struct media_device_ops rkvdec2_media_ops = {
	.req_validate = rkvdec2_request_validate,
	.req_queue = v4l2_m2m_request_queue,
};

static void rkvdec2_job_finish_no_pm(struct rkvdec2_ctx *ctx,
				     enum vb2_buffer_state result)
{
	if (ctx->coded_fmt_desc->ops->done) {
		struct vb2_v4l2_buffer *src_buf, *dst_buf;

		src_buf = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
		dst_buf = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
		ctx->coded_fmt_desc->ops->done(ctx, src_buf, dst_buf, result);
	}

	v4l2_m2m_buf_done_and_job_finish(ctx->dev->m2m_dev, ctx->fh.m2m_ctx,
					 result);
}

static void rkvdec2_job_finish(struct rkvdec2_ctx *ctx,
			       enum vb2_buffer_state result)
{
	struct rkvdec2_dev *rkvdec = ctx->dev;

	pm_runtime_mark_last_busy(rkvdec->dev);
	pm_runtime_put_autosuspend(rkvdec->dev);

	rkvdec2_job_finish_no_pm(ctx, result);
}

void rkvdec2_run_preamble(struct rkvdec2_ctx *ctx, struct rkvdec2_run *run)
{
	struct media_request *src_req;

	memset(run, 0, sizeof(*run));

	run->bufs.src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	run->bufs.dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);

	/* Apply request(s) controls if needed. */
	src_req = run->bufs.src->vb2_buf.req_obj.req;
	if (src_req)
		v4l2_ctrl_request_setup(src_req, &ctx->ctrl_hdl);

	v4l2_m2m_buf_copy_metadata(run->bufs.src, run->bufs.dst, true);
}

void rkvdec2_run_postamble(struct rkvdec2_ctx *ctx, struct rkvdec2_run *run)
{
	struct media_request *src_req = run->bufs.src->vb2_buf.req_obj.req;

	if (src_req)
		v4l2_ctrl_request_complete(src_req, &ctx->ctrl_hdl);
}

static void rkvdec2_device_run(void *priv)
{
	struct rkvdec2_ctx *ctx = priv;
	struct rkvdec2_dev *rkvdec = ctx->dev;
	const struct rkvdec2_coded_fmt_desc *desc = ctx->coded_fmt_desc;
	int ret;

	if (WARN_ON(!desc))
		return;

	ret = pm_runtime_resume_and_get(rkvdec->dev);
	if (ret < 0) {
		rkvdec2_job_finish_no_pm(ctx, VB2_BUF_STATE_ERROR);
		return;
	}

	ret = desc->ops->run(ctx);
	if (ret) {
		cancel_delayed_work(&rkvdec->watchdog_work);
		rkvdec2_job_finish(ctx, VB2_BUF_STATE_ERROR);
	}
}

static const struct v4l2_m2m_ops rkvdec2_m2m_ops = {
	.device_run = rkvdec2_device_run,
};

static int rkvdec2_queue_init(void *priv,
			      struct vb2_queue *src_vq,
			      struct vb2_queue *dst_vq)
{
	struct rkvdec2_ctx *ctx = priv;
	struct rkvdec2_dev *rkvdec = ctx->dev;
	int ret;

	src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	src_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	src_vq->drv_priv = ctx;
	src_vq->ops = &rkvdec2_queue_ops;
	src_vq->mem_ops = &vb2_dma_contig_memops;

	/*
	 * No CPU access on the queues, so no kernel mapping needed.
	 */
	src_vq->dma_attrs = DMA_ATTR_NO_KERNEL_MAPPING;
	src_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src_vq->lock = &rkvdec->vdev_lock;
	src_vq->dev = rkvdec->v4l2_dev.dev;
	src_vq->supports_requests = true;
	src_vq->requires_requests = true;

	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	dst_vq->bidirectional = true;
	dst_vq->mem_ops = &vb2_dma_contig_memops;
	dst_vq->dma_attrs = DMA_ATTR_NO_KERNEL_MAPPING;
	dst_vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	dst_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	dst_vq->drv_priv = ctx;
	dst_vq->ops = &rkvdec2_queue_ops;
	dst_vq->buf_struct_size = sizeof(struct rkvdec2_decoded_buffer);
	dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst_vq->lock = &rkvdec->vdev_lock;
	dst_vq->dev = rkvdec->v4l2_dev.dev;

	return vb2_queue_init(dst_vq);
}

static int rkvdec2_add_ctrls(struct rkvdec2_ctx *ctx,
			     const struct rkvdec2_ctrls *ctrls)
{
	unsigned int i;

	for (i = 0; i < ctrls->num_ctrls; i++) {
		const struct v4l2_ctrl_config *cfg = &ctrls->ctrls[i].cfg;

		v4l2_ctrl_new_custom(&ctx->ctrl_hdl, cfg, ctx);
		if (ctx->ctrl_hdl.error)
			return ctx->ctrl_hdl.error;
	}

	return 0;
}

static int rkvdec2_init_ctrls(struct rkvdec2_ctx *ctx)
{
	unsigned int i, nctrls = 0;
	int ret;

	for (i = 0; i < ARRAY_SIZE(rkvdec2_coded_fmts); i++)
		nctrls += rkvdec2_coded_fmts[i].ctrls->num_ctrls;

	v4l2_ctrl_handler_init(&ctx->ctrl_hdl, nctrls);

	for (i = 0; i < ARRAY_SIZE(rkvdec2_coded_fmts); i++) {
		ret = rkvdec2_add_ctrls(ctx, rkvdec2_coded_fmts[i].ctrls);
		if (ret)
			goto err_free_handler;
	}

	ret = v4l2_ctrl_handler_setup(&ctx->ctrl_hdl);
	if (ret)
		goto err_free_handler;

	ctx->fh.ctrl_handler = &ctx->ctrl_hdl;
	return 0;

err_free_handler:
	v4l2_ctrl_handler_free(&ctx->ctrl_hdl);
	return ret;
}

static int rkvdec2_open(struct file *filp)
{
	struct rkvdec2_dev *rkvdec = video_drvdata(filp);
	struct rkvdec2_ctx *ctx;
	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dev = rkvdec;
	rkvdec2_reset_coded_fmt(ctx);
	rkvdec2_reset_decoded_fmt(ctx);
	v4l2_fh_init(&ctx->fh, video_devdata(filp));

	ret = rkvdec2_init_ctrls(ctx);
	if (ret)
		goto err_free_ctx;

	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(rkvdec->m2m_dev, ctx,
					    rkvdec2_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		goto err_cleanup_ctrls;
	}

	filp->private_data = &ctx->fh;
	v4l2_fh_add(&ctx->fh);

	return 0;

err_cleanup_ctrls:
	v4l2_ctrl_handler_free(&ctx->ctrl_hdl);

err_free_ctx:
	kfree(ctx);
	return ret;
}

static int rkvdec2_release(struct file *filp)
{
	struct rkvdec2_ctx *ctx = fh_to_rkvdec2_ctx(filp->private_data);

	v4l2_fh_del(&ctx->fh);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	v4l2_ctrl_handler_free(&ctx->ctrl_hdl);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);

	return 0;
}

static const struct v4l2_file_operations rkvdec2_fops = {
	.owner = THIS_MODULE,
	.open = rkvdec2_open,
	.release = rkvdec2_release,
	.poll = v4l2_m2m_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = v4l2_m2m_fop_mmap,
};

static int rkvdec2_v4l2_init(struct rkvdec2_dev *rkvdec)
{
	int ret;

	ret = v4l2_device_register(rkvdec->dev, &rkvdec->v4l2_dev);
	if (ret) {
		dev_err(rkvdec->dev, "Failed to register V4L2 device\n");
		return ret;
	}

	rkvdec->m2m_dev = v4l2_m2m_init(&rkvdec2_m2m_ops);
	if (IS_ERR(rkvdec->m2m_dev)) {
		v4l2_err(&rkvdec->v4l2_dev, "Failed to init mem2mem device\n");
		ret = PTR_ERR(rkvdec->m2m_dev);
		goto err_unregister_v4l2;
	}

	rkvdec->mdev.dev = rkvdec->dev;
	strscpy(rkvdec->mdev.model, "rkvdec2", sizeof(rkvdec->mdev.model));
	strscpy(rkvdec->mdev.bus_info, "platform:rkvdec2",
		sizeof(rkvdec->mdev.bus_info));
	media_device_init(&rkvdec->mdev);
	rkvdec->mdev.ops = &rkvdec2_media_ops;
	rkvdec->v4l2_dev.mdev = &rkvdec->mdev;

	rkvdec->vdev.lock = &rkvdec->vdev_lock;
	rkvdec->vdev.v4l2_dev = &rkvdec->v4l2_dev;
	rkvdec->vdev.fops = &rkvdec2_fops;
	rkvdec->vdev.release = video_device_release_empty;
	rkvdec->vdev.vfl_dir = VFL_DIR_M2M;
	rkvdec->vdev.device_caps = V4L2_CAP_STREAMING |
				   V4L2_CAP_VIDEO_M2M_MPLANE;
	rkvdec->vdev.ioctl_ops = &rkvdec2_ioctl_ops;
	video_set_drvdata(&rkvdec->vdev, rkvdec);
	strscpy(rkvdec->vdev.name, "rkvdec2", sizeof(rkvdec->vdev.name));

	ret = video_register_device(&rkvdec->vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		v4l2_err(&rkvdec->v4l2_dev, "Failed to register video device\n");
		goto err_cleanup_mc;
	}

	ret = v4l2_m2m_register_media_controller(rkvdec->m2m_dev, &rkvdec->vdev,
						 MEDIA_ENT_F_PROC_VIDEO_DECODER);
	if (ret) {
		v4l2_err(&rkvdec->v4l2_dev,
			 "Failed to initialize V4L2 M2M media controller\n");
		goto err_unregister_vdev;
	}

	ret = media_device_register(&rkvdec->mdev);
	if (ret) {
		v4l2_err(&rkvdec->v4l2_dev, "Failed to register media device\n");
		goto err_unregister_mc;
	}

	return 0;

err_unregister_mc:
	v4l2_m2m_unregister_media_controller(rkvdec->m2m_dev);

err_unregister_vdev:
	video_unregister_device(&rkvdec->vdev);

err_cleanup_mc:
	media_device_cleanup(&rkvdec->mdev);
	v4l2_m2m_release(rkvdec->m2m_dev);

err_unregister_v4l2:
	v4l2_device_unregister(&rkvdec->v4l2_dev);
	return ret;
}

static void rkvdec2_v4l2_cleanup(struct rkvdec2_dev *rkvdec)
{
	media_device_unregister(&rkvdec->mdev);
	v4l2_m2m_unregister_media_controller(rkvdec->m2m_dev);
	video_unregister_device(&rkvdec->vdev);
	media_device_cleanup(&rkvdec->mdev);
	v4l2_m2m_release(rkvdec->m2m_dev);
	v4l2_device_unregister(&rkvdec->v4l2_dev);
}

static void rkvdec2_iommu_restore(struct rkvdec2_dev *rkvdec)
{
	if (rkvdec->iommu_domain && rkvdec->empty_domain) {
		/* To rewrite mapping into the attached IOMMU core, attach a new empty domain that
		 * will program an empty table, then attach the default domain again to reprogram
		 * all cached mappings.
		 * This is safely done in this interrupt handler to make sure no memory get mapped
		 * through the IOMMU while the empty domain is attached.
		 */
		iommu_attach_device(rkvdec->empty_domain, rkvdec->dev);
		iommu_detach_device(rkvdec->empty_domain, rkvdec->dev);
		iommu_attach_device(rkvdec->iommu_domain, rkvdec->dev);
	}
}

static irqreturn_t rkvdec2_irq_handler(int irq, void *priv)
{
	struct rkvdec2_dev *rkvdec = priv;
	struct rkvdec2_ctx *ctx = v4l2_m2m_get_curr_priv(rkvdec->m2m_dev);
	enum vb2_buffer_state state;
	bool need_reset;
	u32 status;

	status = readl(rkvdec->regs + RKVDEC2_REG_STA_INT);
	state = (status & STA_INT_DEC_RDY_STA) ?
		VB2_BUF_STATE_DONE : VB2_BUF_STATE_ERROR;

	need_reset = state != VB2_BUF_STATE_DONE ||
			      (status & STA_INT_SOFTRESET_RDY);

	/* Clear interrupt status */
	writel(0, rkvdec->regs + RKVDEC2_REG_STA_INT);

	if (need_reset)
		rkvdec2_iommu_restore(rkvdec);

	if (cancel_delayed_work(&rkvdec->watchdog_work))
		rkvdec2_job_finish(ctx, state);

	return IRQ_HANDLED;
}

static void rkvdec2_watchdog_func(struct work_struct *work)
{
	struct rkvdec2_dev *rkvdec = container_of(to_delayed_work(work), struct rkvdec2_dev,
			      watchdog_work);
	struct rkvdec2_ctx *ctx = v4l2_m2m_get_curr_priv(rkvdec->m2m_dev);

	if (ctx) {
		dev_err(rkvdec->dev, "Frame processing timed out!\n");
		writel(RKVDEC2_REG_DEC_IRQ_DISABLE, rkvdec->regs + RKVDEC2_REG_IMPORTANT_EN);
		writel(0, rkvdec->regs + RKVDEC2_REG_DEC_E);
		rkvdec2_job_finish(ctx, VB2_BUF_STATE_ERROR);
	}
}

static const struct of_device_id of_rkvdec2_match[] = {
	{ .compatible = "rockchip,rk3588-vdec" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_rkvdec2_match);

/*
 * Some SoCs, like RK3588 have multiple identical vdpu34x cores, but the
 * kernel is currently missing support for multi-core handling. Exposing
 * separate devices for each core to userspace is bad, since that does
 * not allow scheduling tasks properly (and creates ABI). With this workaround
 * the driver will only probe for the first core and early exit for the other
 * cores. Once the driver gains multi-core support, the same technique
 * can be used to cluster all cores together in one linux device.
 */
static int rkvdec2_disable_multicore(struct rkvdec2_dev *rkvdec)
{
	const char *compatible;
	struct device_node *node;
	int ret;

	/* Intentionally ignores the fallback strings */
	ret = of_property_read_string(rkvdec->dev->of_node, "compatible", &compatible);
	if (ret)
		return ret;

	/* first compatible node found from the root node is considered the main core */
	node = of_find_compatible_node(NULL, NULL, compatible);
	if (!node)
		return -EINVAL; /* broken DT? */

	if (rkvdec->dev->of_node != node) {
		dev_info(rkvdec->dev, "missing multi-core support, ignoring this instance\n");
		return -ENODEV;
	}

	return 0;
}

static int rkvdec2_probe(struct platform_device *pdev)
{
	struct rkvdec2_dev *rkvdec;
	unsigned int dma_bit_mask = 40;
	int ret, irq;

	rkvdec = devm_kzalloc(&pdev->dev, sizeof(*rkvdec), GFP_KERNEL);
	if (!rkvdec)
		return -ENOMEM;

	platform_set_drvdata(pdev, rkvdec);
	rkvdec->dev = &pdev->dev;

	ret = rkvdec2_disable_multicore(rkvdec);
	if (ret)
		return ret;

	mutex_init(&rkvdec->vdev_lock);
	INIT_DELAYED_WORK(&rkvdec->watchdog_work, rkvdec2_watchdog_func);

	ret = devm_clk_bulk_get_all_enabled(&pdev->dev, &rkvdec->clocks);
	if (ret < 0)
		return ret;

	rkvdec->clk_count = ret;
	rkvdec->axi_clk = devm_clk_get(&pdev->dev, "axi");

	rkvdec->regs = devm_platform_ioremap_resource_byname(pdev, "function");
	if (IS_ERR(rkvdec->regs))
		return PTR_ERR(rkvdec->regs);

	irq = platform_get_irq(pdev, 0);
	if (irq <= 0)
		return -ENXIO;

	ret = devm_request_threaded_irq(&pdev->dev, irq, NULL,
					rkvdec2_irq_handler, IRQF_ONESHOT,
					dev_name(&pdev->dev), rkvdec);
	if (ret) {
		dev_err(&pdev->dev, "Could not request vdec2 IRQ\n");
		return ret;
	}

	rkvdec->iommu_domain = iommu_get_domain_for_dev(&pdev->dev);
	if (!rkvdec->iommu_domain) {
		/* Without iommu, only the lower 32 bits of ram can be used */
		vb2_dma_contig_set_max_seg_size(&pdev->dev, U32_MAX);
		dev_info(&pdev->dev, "No IOMMU domain found\n");
	} else {
		rkvdec->empty_domain = iommu_paging_domain_alloc(rkvdec->dev);

		if (!rkvdec->empty_domain)
			dev_warn(rkvdec->dev, "cannot alloc new empty domain\n");
	}

	ret = dma_set_coherent_mask(&pdev->dev, DMA_BIT_MASK(dma_bit_mask));
	if (ret) {
		dev_err(&pdev->dev, "Could not set DMA coherent mask.\n");
		return ret;
	}

	rkvdec->sram_pool = of_gen_pool_get(pdev->dev.of_node, "sram", 0);
	if (!rkvdec->sram_pool)
		dev_info(&pdev->dev, "No sram node, RCB will be stored in RAM\n");

	pm_runtime_set_autosuspend_delay(&pdev->dev, 100);
	pm_runtime_use_autosuspend(&pdev->dev);
	pm_runtime_enable(&pdev->dev);

	ret = rkvdec2_v4l2_init(rkvdec);
	if (ret)
		goto err_disable_runtime_pm;

	return 0;

err_disable_runtime_pm:
	pm_runtime_dont_use_autosuspend(&pdev->dev);
	pm_runtime_disable(&pdev->dev);

	if (rkvdec->sram_pool)
		gen_pool_destroy(rkvdec->sram_pool);

	return ret;
}

static void rkvdec2_remove(struct platform_device *pdev)
{
	struct rkvdec2_dev *rkvdec = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&rkvdec->watchdog_work);

	rkvdec2_v4l2_cleanup(rkvdec);
	pm_runtime_disable(&pdev->dev);
	pm_runtime_dont_use_autosuspend(&pdev->dev);

	if (rkvdec->sram_pool)
		gen_pool_destroy(rkvdec->sram_pool);

	if (rkvdec->empty_domain)
		iommu_domain_free(rkvdec->empty_domain);
}

#ifdef CONFIG_PM
static int rkvdec2_runtime_resume(struct device *dev)
{
	struct rkvdec2_dev *rkvdec = dev_get_drvdata(dev);

	return clk_bulk_prepare_enable(rkvdec->clk_count,
				       rkvdec->clocks);
}

static int rkvdec2_runtime_suspend(struct device *dev)
{
	struct rkvdec2_dev *rkvdec = dev_get_drvdata(dev);

	clk_bulk_disable_unprepare(rkvdec->clk_count,
				   rkvdec->clocks);
	return 0;
}
#endif

static const struct dev_pm_ops rkvdec2_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(rkvdec2_runtime_suspend, rkvdec2_runtime_resume, NULL)
};

static struct platform_driver rkvdec2_driver = {
	.probe = rkvdec2_probe,
	.remove = rkvdec2_remove,
	.driver = {
		   .name = "rkvdec2",
		   .of_match_table = of_rkvdec2_match,
		   .pm = &rkvdec2_pm_ops,
	},
};
module_platform_driver(rkvdec2_driver);

MODULE_AUTHOR("Detlev Casanova <detlev.casanova@collabora.com>");
MODULE_DESCRIPTION("Rockchip Video Decoder 2 driver");
MODULE_LICENSE("GPL");
