FATE_HEVC = cainit_a_sharp_4        \
            cainit_b_sharp_4        \
            cainit_c_sharp_3        \
            cainit_d_sharp_3        \
            cainit_e_sharp_3        \
            cainit_f_sharp_3        \
            cainit_g_sharp_3        \
            cainit_h_sharp_3        \
            cip_b_nec_2             \
            dslice_a_hhi_5          \
            dslice_b_hhi_5          \
            dslice_c_hhi_5          \
            entp_a_lg_2             \
            entp_b_lg_2             \
            entp_c_lg_3             \
            ext_a_ericsson_3        \
            ipcm_a_nec_2            \
            ipcm_b_nec_2            \
            ipred_a_docomo_2        \
            ipred_b_nokia_3         \
            ipred_c_mitsubishi_2    \
            maxbins_a_ti_4          \
            maxbins_b_ti_4          \
            maxbins_c_ti_4          \
            merge_a_ti_3            \
            merge_b_ti_3            \
            merge_c_ti_3            \
            merge_d_ti_3            \
            merge_e_ti_3            \
            merge_g_hhi_4           \
            mvclip_a_qualcomm_3     \
            mvdl1zero_a_docomo_3    \
            mvedge_a_qualcomm_3     \
            nut_a_ericsson_4        \
            pmerge_a_ti_3           \
            pmerge_b_ti_3           \
            pmerge_c_ti_3           \
            pmerge_d_ti_3           \
            pmerge_e_ti_3           \
            ps_a_vidyo_3            \
            ps_b_vidyo_3            \
            rap_a_docomo_4          \
            rplm_a_qualcomm_4       \
            rps_b_qualcomm_5        \
            rps_c_ericsson_4        \
            rps_d_ericsson_5        \
            rps_e_qualcomm_5        \
            rqt_a_hhi_4             \
            rqt_b_hhi_4             \
            rqt_c_hhi_4             \
            rqt_d_hhi_4             \
            rqt_e_hhi_4             \
            rqt_f_hhi_4             \
            rqt_g_hhi_4             \
            sao_a_mediatek_4        \
            sao_b_mediatek_5        \
            sao_c_samsung_4         \
            sao_d_samsung_4         \
            tscl_a_vidyo_5          \
            tscl_b_vidyo_4          \
            tskip_a_ms_2            \
            wp_a_toshiba_3          \
            wp_b_toshiba_3          \
            wpp_a_ericsson_main_2   \
            wpp_b_ericsson_main_2   \
            wpp_c_ericsson_main_2   \
            wpp_e_ericsson_main_2   \
            wpp_f_ericsson_main_2   \

FATE_HEVC := $(FATE_HEVC:%=fate-hevc-conformance-%)

FATE_HEVC-$(call DEMDEC, HEVC, HEVC) += $(FATE_HEVC)

FATE_SAMPLES_AVCONV += $(FATE_HEVC-yes)

fate-hevc: $(FATE_HEVC-yes)

fate-hevc-conformance-cainit_a_sharp_4:      CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/CAINIT_A_SHARP_4.bit
fate-hevc-conformance-cainit_b_sharp_4:      CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/CAINIT_B_SHARP_4.bit
fate-hevc-conformance-cainit_c_sharp_3:      CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/CAINIT_C_SHARP_3.bit
fate-hevc-conformance-cainit_d_sharp_3:      CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/CAINIT_D_SHARP_3.bit
fate-hevc-conformance-cainit_e_sharp_3:      CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/CAINIT_E_SHARP_3.bit
fate-hevc-conformance-cainit_f_sharp_3:      CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/CAINIT_F_SHARP_3.bit
fate-hevc-conformance-cainit_g_sharp_3:      CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/CAINIT_G_SHARP_3.bit
fate-hevc-conformance-cainit_h_sharp_3:      CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/CAINIT_H_SHARP_3.bit
fate-hevc-conformance-cip_b_nec_2:           CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/cip_B_NEC_2.bit
fate-hevc-conformance-dslice_a_hhi_5:        CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/DSLICE_A_HHI_5.bin
fate-hevc-conformance-dslice_b_hhi_5:        CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/DSLICE_B_HHI_5.bin
fate-hevc-conformance-dslice_c_hhi_5:        CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/DSLICE_C_HHI_5.bin
fate-hevc-conformance-entp_a_lg_2:           CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/ENTP_A_LG_2.bin
fate-hevc-conformance-entp_b_lg_2:           CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/ENTP_B_LG_2.bin
fate-hevc-conformance-entp_c_lg_3:           CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/ENTP_C_LG_3.bin
fate-hevc-conformance-ext_a_ericsson_3:      CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/EXT_A_ericsson_3.bit
fate-hevc-conformance-ipcm_a_nec_2:          CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/ipcm_A_NEC_2.bit
fate-hevc-conformance-ipcm_b_nec_2:          CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/ipcm_B_NEC_2.bit
fate-hevc-conformance-ipred_a_docomo_2:      CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/IPRED_A_docomo_2.bit
fate-hevc-conformance-ipred_b_nokia_3:       CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/IPRED_B_Nokia_3.bit
fate-hevc-conformance-ipred_c_mitsubishi_2:  CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/IPRED_C_Mitsubishi_2.bit
fate-hevc-conformance-maxbins_a_ti_4:        CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/MAXBINS_A_TI_4.bit
fate-hevc-conformance-maxbins_b_ti_4:        CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/MAXBINS_B_TI_4.bit
fate-hevc-conformance-maxbins_c_ti_4:        CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/MAXBINS_C_TI_4.bit
fate-hevc-conformance-merge_a_ti_3:          CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/MERGE_A_TI_3.bit
fate-hevc-conformance-merge_b_ti_3:          CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/MERGE_B_TI_3.bit
fate-hevc-conformance-merge_c_ti_3:          CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/MERGE_C_TI_3.bit
fate-hevc-conformance-merge_d_ti_3:          CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/MERGE_D_TI_3.bit
fate-hevc-conformance-merge_e_ti_3:          CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/MERGE_E_TI_3.bit
fate-hevc-conformance-merge_g_hhi_4:         CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/MERGE_G_HHI_4.bit
fate-hevc-conformance-mvclip_a_qualcomm_3:   CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/MVCLIP_A_qualcomm_3.bit
fate-hevc-conformance-mvdl1zero_a_docomo_3:  CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/MVDL1ZERO_A_docomo_3.bit
fate-hevc-conformance-mvedge_a_qualcomm_3:   CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/MVEDGE_A_qualcomm_3.bit
fate-hevc-conformance-nut_a_ericsson_4:      CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/NUT_A_ericsson_4.bit
fate-hevc-conformance-pmerge_a_ti_3:         CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/PMERGE_A_TI_3.bit
fate-hevc-conformance-pmerge_b_ti_3:         CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/PMERGE_B_TI_3.bit
fate-hevc-conformance-pmerge_c_ti_3:         CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/PMERGE_C_TI_3.bit
fate-hevc-conformance-pmerge_d_ti_3:         CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/PMERGE_D_TI_3.bit
fate-hevc-conformance-pmerge_e_ti_3:         CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/PMERGE_E_TI_3.bit
fate-hevc-conformance-ps_a_vidyo_3:          CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/PS_A_VIDYO_3.bit
fate-hevc-conformance-ps_b_vidyo_3:          CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/PS_B_VIDYO_3.bit
fate-hevc-conformance-rap_a_docomo_4:        CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/RAP_A_docomo_4.bit
fate-hevc-conformance-rplm_a_qualcomm_4:     CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/RPLM_A_qualcomm_4.bit
fate-hevc-conformance-rps_b_qualcomm_5:      CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/RPS_B_qualcomm_5.bit
fate-hevc-conformance-rps_c_ericsson_4:      CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/RPS_C_ericsson_4.bit
fate-hevc-conformance-rps_d_ericsson_5:      CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/RPS_D_ericsson_5.bit
fate-hevc-conformance-rps_e_qualcomm_5:      CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/RPS_E_qualcomm_5.bit
fate-hevc-conformance-rqt_a_hhi_4:           CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/RQT_A_HHI_4.bit
fate-hevc-conformance-rqt_b_hhi_4:           CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/RQT_B_HHI_4.bit
fate-hevc-conformance-rqt_c_hhi_4:           CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/RQT_C_HHI_4.bit
fate-hevc-conformance-rqt_d_hhi_4:           CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/RQT_D_HHI_4.bit
fate-hevc-conformance-rqt_e_hhi_4:           CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/RQT_E_HHI_4.bit
fate-hevc-conformance-rqt_f_hhi_4:           CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/RQT_F_HHI_4.bit
fate-hevc-conformance-rqt_g_hhi_4:           CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/RQT_G_HHI_4.bit
fate-hevc-conformance-sao_a_mediatek_4:      CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/SAO_A_MediaTek_4.bit
fate-hevc-conformance-sao_b_mediatek_5:      CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/SAO_B_MediaTek_5.bit
fate-hevc-conformance-sao_c_samsung_4:       CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/SAO_C_Samsung_4.bin
fate-hevc-conformance-sao_d_samsung_4:       CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/SAO_D_Samsung_4.bin
fate-hevc-conformance-tscl_a_vidyo_5:        CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/TSCL_A_VIDYO_5.bit
fate-hevc-conformance-tscl_b_vidyo_4:        CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/TSCL_B_VIDYO_4.bit
fate-hevc-conformance-tskip_a_ms_2:          CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/TSKIP_A_MS_2.bit
fate-hevc-conformance-wp_a_toshiba_3:        CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/WP_A_Toshiba_3.bit
fate-hevc-conformance-wp_b_toshiba_3:        CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/WP_B_Toshiba_3.bit
fate-hevc-conformance-wpp_a_ericsson_main_2: CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/WPP_A_ericsson_MAIN_2.bit
fate-hevc-conformance-wpp_b_ericsson_main_2: CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/WPP_B_ericsson_MAIN_2.bit
fate-hevc-conformance-wpp_c_ericsson_main_2: CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/WPP_C_ericsson_MAIN_2.bit
fate-hevc-conformance-wpp_e_ericsson_main_2: CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/WPP_E_ericsson_MAIN_2.bit
fate-hevc-conformance-wpp_f_ericsson_main_2: CMD = explode -f hevc -decode-checksum 1 -i $(TARGET_SAMPLES)/hevc-conformance/WPP_F_ericsson_MAIN_2.bit
