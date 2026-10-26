/******************************************************************************
 *
 * Copyright(c) 2007 - 2017 Realtek Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 *****************************************************************************/
#define _HCI_OPS_OS_C_

/* #include <drv_types.h> */
#include <rtl8814a_hal.h>

#ifdef CONFIG_SUPPORT_USB_INT
void interrupt_handler_8814au(_adapter *padapter, u16 pkt_len, u8 *pbuf)
{
	HAL_DATA_TYPE	*pHalData = GET_HAL_DATA(padapter);
	struct reportpwrstate_parm pwr_rpt;

	if (pkt_len != INTERRUPT_MSG_FORMAT_LEN) {
		RTW_INFO("%s Invalid interrupt content length (%d)!\n", __FUNCTION__, pkt_len);
		return ;
	}

	/* HISR */
	_rtw_memcpy(&(pHalData->IntArray[0]), &(pbuf[USB_INTR_CONTENT_HISR_OFFSET]), 4);
	_rtw_memcpy(&(pHalData->IntArray[1]), &(pbuf[USB_INTR_CONTENT_HISRE_OFFSET]), 4);

#if 0 /*DBG*/
	{
		u32 hisr = 0 , hisr_ex = 0;
		_rtw_memcpy(&hisr, &(pHalData->IntArray[0]), 4);
		hisr = le32_to_cpu(hisr);

		_rtw_memcpy(&hisr_ex, &(pHalData->IntArray[1]), 4);
		hisr_ex = le32_to_cpu(hisr_ex);

		if ((hisr != 0) || (hisr_ex != 0))
			RTW_INFO("===> %s hisr:0x%08x ,hisr_ex:0x%08x\n", __FUNCTION__, hisr, hisr_ex);
	}
#endif

#ifdef CONFIG_LPS_LCLK
	if (pHalData->IntArray[0]  & IMR_CPWM_88E) {
		_rtw_memcpy(&pwr_rpt.state, &(pbuf[USB_INTR_CONTENT_CPWM1_OFFSET]), 1);
		/* _rtw_memcpy(&pwr_rpt.state2, &(pbuf[USB_INTR_CONTENT_CPWM2_OFFSET]), 1); */

		/* 88e's cpwm value only change BIT0, so driver need to add PS_STATE_S2 for LPS flow.		 */
		pwr_rpt.state |= PS_STATE_S2;
		_set_workitem(&(adapter_to_pwrctl(padapter)->cpwm_event));
	}
#endif/* CONFIG_LPS_LCLK */

#ifdef CONFIG_INTERRUPT_BASED_TXBCN

#ifdef CONFIG_INTERRUPT_BASED_TXBCN_EARLY_INT
	if (pHalData->IntArray[0] & IMR_BCNDMAINT0_88E)
#endif
#ifdef CONFIG_INTERRUPT_BASED_TXBCN_BCN_OK_ERR
		if (pHalData->IntArray[0] & (IMR_TBDER_88E | IMR_TBDOK_88E))
#endif
		{
			struct mlme_priv *pmlmepriv = &padapter->mlmepriv;

			if(check_fwstate(pmlmepriv, WIFI_AP_STATE))
			{
				//send_beacon(padapter);
				if(pmlmepriv->update_bcn == _TRUE)
				{
					//tx_beacon_hdl(padapter, NULL);
					set_tx_beacon_cmd(padapter);
				}
			}
#ifdef CONFIG_CONCURRENT_MODE
			if(check_buddy_fwstate(padapter, WIFI_AP_STATE))
			{
				//send_beacon(padapter);
				if(padapter->pbuddy_adapter->mlmepriv.update_bcn == _TRUE)
				{
					//tx_beacon_hdl(padapter, NULL);
					set_tx_beacon_cmd(padapter->pbuddy_adapter);
				}
			}
#endif

		}
#endif /* CONFIG_INTERRUPT_BASED_TXBCN */




#ifdef DBG_CONFIG_ERROR_DETECT_INT
	if (pHalData->IntArray[1]  & IMR_TXERR_88E)
		RTW_INFO("===> %s Tx Error Flag Interrupt Status\n", __FUNCTION__);
	if (pHalData->IntArray[1]  & IMR_RXERR_88E)
		RTW_INFO("===> %s Rx Error Flag INT Status\n", __FUNCTION__);
	if (pHalData->IntArray[1]  & IMR_TXFOVW_88E)
		RTW_INFO("===> %s Transmit FIFO Overflow\n", __FUNCTION__);
	if (pHalData->IntArray[1]  & IMR_RXFOVW_88E)
		RTW_INFO("===> %s Receive FIFO Overflow\n", __FUNCTION__);
#endif/* DBG_CONFIG_ERROR_DETECT_INT */

#ifdef CONFIG_FW_C2H_REG
	/* C2H Event */
	if (pbuf[0] != 0)
		usb_c2h_hisr_hdl(padapter, pbuf);
#endif
}
#endif /* CONFIG_SUPPORT_USB_INT */

void rtl8814au_xmit_tasklet(struct tasklet_struct *t)
{
	int ret = _FALSE;
	struct xmit_priv *pxmitpriv = from_tasklet(pxmitpriv, t, xmit_tasklet);
	_adapter *padapter = pxmitpriv->adapter;

	while(1)
	{
		if (RTW_CANNOT_TX(padapter))
		{
			RTW_INFO("xmit_tasklet => bDriverStopped or bSurpriseRemoved or bWritePortCancel\n");
			break;
		}

		if (rtw_xmit_ac_blocked(padapter) == _TRUE)
			break;

		ret = rtl8814au_xmitframe_complete(padapter, pxmitpriv, NULL);

		if(ret==_FALSE)
			break;

	}

}

void rtl8814au_set_intf_ops(struct _io_ops	*pops)
{
	_rtw_memset((u8 *)pops, 0, sizeof(struct _io_ops));

	pops->_read8 = &usb_read8;
	pops->_read16 = &usb_read16;
	pops->_read32 = &usb_read32;
	pops->_read_mem = &usb_read_mem;
	pops->_read_port = &usb_read_port;

	pops->_write8 = &usb_write8;
	pops->_write16 = &usb_write16;
	pops->_write32 = &usb_write32;
	pops->_writeN = &usb_writeN;

#ifdef CONFIG_USB_SUPPORT_ASYNC_VDN_REQ
	pops->_write8_async= &usb_async_write8;
	pops->_write16_async = &usb_async_write16;
	pops->_write32_async = &usb_async_write32;
#endif
	pops->_write_mem = &usb_write_mem;
	pops->_write_port = &usb_write_port;

	pops->_read_port_cancel = &usb_read_port_cancel;
	pops->_write_port_cancel = &usb_write_port_cancel;

#ifdef CONFIG_USB_INTERRUPT_IN_PIPE
	pops->_read_interrupt = &usb_read_interrupt;
#endif

}

void rtl8814au_set_hw_type(struct dvobj_priv *pdvobj)
{
	pdvobj->HardwareType = HARDWARE_TYPE_RTL8814AU;
	RTW_INFO("CHIP TYPE: RTL8814\n");
}
