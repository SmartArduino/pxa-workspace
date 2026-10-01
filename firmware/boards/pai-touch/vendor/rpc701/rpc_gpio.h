#pragma once

#define RPC_GROUP_NUM 		16


#define RPC_PORTA_00 				(RPC_GROUP_NUM * 0 + 0)
#define RPC_PORTA_01 				(RPC_GROUP_NUM * 0 + 1)
#define RPC_PORTA_02 				(RPC_GROUP_NUM * 0 + 2)
#define RPC_PORTA_03 				(RPC_GROUP_NUM * 0 + 3)
#define RPC_PORTA_04 				(RPC_GROUP_NUM * 0 + 4)
#define RPC_PORTA_05 				(RPC_GROUP_NUM * 0 + 5)
#define RPC_PORTA_06 				(RPC_GROUP_NUM * 0 + 6)
#define RPC_PORTA_07 				(RPC_GROUP_NUM * 0 + 7)
#define RPC_PORTA_08 				(RPC_GROUP_NUM * 0 + 8)
#define RPC_PORTA_09 				(RPC_GROUP_NUM * 0 + 9)
#define RPC_PORTA_10 				(RPC_GROUP_NUM * 0 + 10)
#define RPC_PORTA_11 				(RPC_GROUP_NUM * 0 + 11)
#define RPC_PORTA_12 				(RPC_GROUP_NUM * 0 + 12)
#define RPC_PORTA_13 				(RPC_GROUP_NUM * 0 + 13)
#define RPC_PORTA_14 				(RPC_GROUP_NUM * 0 + 14)
#define RPC_PORTA_15 				(RPC_GROUP_NUM * 0 + 15)

#define RPC_PORTB_00 				(RPC_GROUP_NUM * 1 + 0)
#define RPC_PORTB_01 				(RPC_GROUP_NUM * 1 + 1)
#define RPC_PORTB_02 				(RPC_GROUP_NUM * 1 + 2)
#define RPC_PORTB_03 				(RPC_GROUP_NUM * 1 + 3)
#define RPC_PORTB_04 				(RPC_GROUP_NUM * 1 + 4)
#define RPC_PORTB_05 				(RPC_GROUP_NUM * 1 + 5)
#define RPC_PORTB_06 				(RPC_GROUP_NUM * 1 + 6)
#define RPC_PORTB_07 				(RPC_GROUP_NUM * 1 + 7)
#define RPC_PORTB_08 				(RPC_GROUP_NUM * 1 + 8)
#define RPC_PORTB_09 				(RPC_GROUP_NUM * 1 + 9)
#define RPC_PORTB_10 				(RPC_GROUP_NUM * 1 + 10)
#define RPC_PORTB_11 				(RPC_GROUP_NUM * 1 + 11)

#define RPC_PORTC_00 				(RPC_GROUP_NUM * 2 + 0)
#define RPC_PORTC_01 				(RPC_GROUP_NUM * 2 + 1)
#define RPC_PORTC_02 				(RPC_GROUP_NUM * 2 + 2)
#define RPC_PORTC_03 				(RPC_GROUP_NUM * 2 + 3)
#define RPC_PORTC_04 				(RPC_GROUP_NUM * 2 + 4)
#define RPC_PORTC_05 				(RPC_GROUP_NUM * 2 + 5)
#define RPC_PORTC_06 				(RPC_GROUP_NUM * 2 + 6)
#define RPC_PORTC_07 				(RPC_GROUP_NUM * 2 + 7)
#define RPC_PORTC_08 				(RPC_GROUP_NUM * 2 + 8)

#define RPC_PORTD_00 				(RPC_GROUP_NUM * 3 + 0)
#define RPC_PORTD_01 				(RPC_GROUP_NUM * 3 + 1)
#define RPC_PORTD_02 				(RPC_GROUP_NUM * 3 + 2)
#define RPC_PORTD_03 				(RPC_GROUP_NUM * 3 + 3)
#define RPC_PORTD_04 				(RPC_GROUP_NUM * 3 + 4)
#define RPC_PORTD_05 				(RPC_GROUP_NUM * 3 + 5)
#define RPC_PORTD_06 				(RPC_GROUP_NUM * 3 + 6)

#define RPC_PORTE_00 				(RPC_GROUP_NUM * 4 + 0)
#define RPC_PORTE_01 				(RPC_GROUP_NUM * 4 + 1)
#define RPC_PORTE_02 				(RPC_GROUP_NUM * 4 + 2)
#define RPC_PORTE_03 				(RPC_GROUP_NUM * 4 + 3)
#define RPC_PORTE_04 				(RPC_GROUP_NUM * 4 + 4)
#define RPC_PORTE_05 				(RPC_GROUP_NUM * 4 + 5)
#define RPC_PORTE_06 				(RPC_GROUP_NUM * 4 + 6)

#define RPC_PORTG_00 				(RPC_GROUP_NUM * 5 + 0)
#define RPC_PORTG_01 				(RPC_GROUP_NUM * 5 + 1)
#define RPC_PORTG_02 				(RPC_GROUP_NUM * 5 + 2)
#define RPC_PORTG_03 				(RPC_GROUP_NUM * 5 + 3)
#define RPC_PORTG_04 				(RPC_GROUP_NUM * 5 + 4)
#define RPC_PORTG_05 				(RPC_GROUP_NUM * 5 + 5)
#define RPC_PORTG_06 				(RPC_GROUP_NUM * 5 + 6)
#define RPC_PORTG_07 				(RPC_GROUP_NUM * 5 + 7)
#define RPC_PORTG_08 				(RPC_GROUP_NUM * 5 + 8)

#define RPC_PORTP_00 				(RPC_GROUP_NUM * 6 + 0)

#define RPC_MAX_NUM 					(RPC_PORTP_00 + 1)

#define RPC_PORT_PR_00               (RPC_MAX_NUM + 0)
#define RPC_PORT_PR_01               (RPC_MAX_NUM + 1)
#define RPC_PORT_PR_02               (RPC_MAX_NUM + 2)
#define RPC_PORT_PR_03               (RPC_MAX_NUM + 3)
#define RPC_PORT_PR_04               (RPC_MAX_NUM + 4)

#define USB_RPC_OFFSET               5
#define RPC_PORT_DP                  (RPC_MAX_NUM + USB_RPC_OFFSET)
#define RPC_PORT_DM                  (RPC_MAX_NUM + USB_RPC_OFFSET + 1)

#define P33_RPC_OFFSET               7
#define RPC_CHGFL_DET                (RPC_MAX_NUM + P33_RPC_OFFSET + 0)
#define RPC_VBGOK_DET                (RPC_MAX_NUM + P33_RPC_OFFSET + 1)
#define RPC_VBTCH_DET                (RPC_MAX_NUM + P33_RPC_OFFSET + 2)
#define RPC_LDOIN_DET                (RPC_MAX_NUM + P33_RPC_OFFSET + 3)

#define RPC_PORT_MAX					(RPC_PORT_DM + 1)
