include(`audioreach/audioreach.m4')
include(`audioreach/stream-subgraph.m4')
include(`audioreach/device-subgraph.m4')
include(`util/route.m4')
include(`util/mixer.m4')
include(`audioreach/tokens.m4')

STREAM_SG_PCM_ADD(piano/subgraph-stream-playback.m4, FRONTEND_DAI_MULTIMEDIA1,
	`S16_LE', 48000, 48000, 1, 2,
	0x00004001, 0x00004001, 0x00006001, `110000')

DEVICE_AUDIO_IF_SG_ADD(`subgraph-device-tdm-2ch-mfc.m4',
	`Secondary TDM0', SECONDARY_TDM_RX_0,
	`S32_LE', 48000, 48000, 2, 2,
	LPAIF_INTF_TYPE_LPAIF, I2S_INTF_TYPE_SECONDARY, 0, DATA_FORMAT_FIXED_POINT,
	0x00004005, 0x00004005, 0x00006050, `SECONDARY_TDM_RX_0',
	AUDIO_IF_SYNC_SRC_INTERNAL, AUDIO_IF_CTRL_DATA_OE_ENABLE,
	0x3, 4, 32,
	0, 1, 0, 1,
	0, AUDIO_IF_LANE_MASK_1, 0,
	0, 0, 0)

STREAM_DEVICE_PLAYBACK_MIXER(SECONDARY_TDM_RX_0, ``SECONDARY_TDM_RX_0'', ``MultiMedia1'')
STREAM_DEVICE_PLAYBACK_ROUTE(SECONDARY_TDM_RX_0, ``SECONDARY_TDM_RX_0 Audio Mixer'', ``MultiMedia1, stream0.logger1'')
