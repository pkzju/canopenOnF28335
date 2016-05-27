/*
This file is part of CanFestival, a library implementing CanOpen Stack.

Copyright (C): Edouard TISSERANT and Francis DUPIN
C2000 Port: Michael LUTZ

See COPYING file for copyrights details.

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

//#define DEBUG_WAR_CONSOLE_ON
//#define DEBUG_ERR_CONSOLE_ON

#include <DSP2833x_Device.h>
#include "canfestival.h"

struct Ecan {
	volatile struct ECAN_REGS *ECanRegs;
	volatile struct ECAN_MBOXES *ECanMboxes;
	volatile struct LAM_REGS *ECanLAMRegs;
	volatile struct MOTO_REGS *ECanMOTORegs;
	volatile struct MOTS_REGS *ECanMOTSRegs;
	CO_Data *co;
};

static struct Ecan _can_ports[] = {
	{&ECanaRegs, &ECanaMboxes, &ECanaLAMRegs, &ECanaMOTORegs, &ECanaMOTSRegs},
	{&ECanbRegs, &ECanbMboxes, &ECanbLAMRegs, &ECanbMOTORegs, &ECanbMOTSRegs},
};

volatile int _in_int = 0;

#define CAN_MB 32
#define CAN_TX_MB 16
#define CAN_RX_MB (CAN_MB - CAN_TX_MB)
#define CAN_SPECIAL_RX_MB 6 /* NMT + TIME + 2xSYNC + 2xNODEGUARD */

#pragma CODE_SECTION(can_irq_mbox_handler_a, "ramfuncs");
interrupt void can_irq_mbox_handler_a(void);
#pragma CODE_SECTION(can_irq_mbox_handler_b, "ramfuncs");
interrupt void can_irq_mbox_handler_b(void);

/* Initialize a CAN port for use with CANfestival. */
CAN_PORT canInit(int port, CO_Data *ObjDict_Data, unsigned long bitrate)
{
	CAN_PORT p = &_can_ports[port];struct ECAN_REGS ECanShadow;

	if (!canSetBitrate(p, bitrate)) return NULL;

	p->co = ObjDict_Data;

	EALLOW;  // This is needed to write to EALLOW protected registers.

	// Configure the eCAN RX and TX pins for eCAN transmissions
    ECanShadow.CANTIOC.all = ECanbRegs.CANTIOC.all;
    ECanShadow.CANTIOC.bit.TXFUNC = 1;
    p->ECanRegs->CANTIOC.all = ECanShadow.CANTIOC.all;

    ECanShadow.CANRIOC.all = ECanbRegs.CANRIOC.all;
    ECanShadow.CANRIOC.bit.RXFUNC = 1;
    p->ECanRegs->CANTIOC.all = ECanShadow.CANRIOC.all;

	volatile union CANLAM_REG *lam = &p->ECanLAMRegs->LAM0;
	volatile struct MBOX *mbox = &p->ECanMboxes->MBOX0;
	p->ECanRegs->CANME.all = 0;

	// Configure transmit mailboxes.
	int i;
	for (i = 0; i < CAN_TX_MB; i++, lam++, mbox++) {
		mbox->MSGID.all = 0;
	}
	// Configure remaining mailboxes for receiving.
	for (; i < CAN_MB; i++, lam++, mbox++) {
		lam->all = 0xFFFFFFFF;        // Accept any message ID.
		mbox->MSGID.all = 0x40000000;
	}

	Uint32 mask = ((1UL << CAN_RX_MB) - 1) << CAN_TX_MB;
	p->ECanRegs->CANMD.all  = mask;
	p->ECanRegs->CANOPC.all = mask & (mask - 1); // Last receive buffer is a catch-all.
	p->ECanRegs->CANME.all  = mask;

	if (port == 0) {
		PieVectTable.ECAN0INTA = &can_irq_mbox_handler_a;
		PieVectTable.ECAN1INTA = &can_irq_mbox_handler_a;
		PieCtrlRegs.PIEIER9.bit.INTx5 = 1;
		PieCtrlRegs.PIEIER9.bit.INTx6 = 1;
	} else {
		PieVectTable.ECAN0INTB = &can_irq_mbox_handler_b;
		PieVectTable.ECAN1INTB = &can_irq_mbox_handler_b;
		PieCtrlRegs.PIEIER9.bit.INTx7 = 1;
		PieCtrlRegs.PIEIER9.bit.INTx8 = 1;
	}

	p->ECanRegs->CANMIL.all = 0xFFFFFFFF;
	p->ECanRegs->CANMIM.all = 0xFFFFFFFF;
	p->ECanRegs->CANGIM.all = 0x00000002;

    ECanShadow.CANMC.all = ECanbRegs.CANMC.all;
    ECanShadow.CANMC.bit.SCB = 1;    // eCAN mode (reqd to access 32 mailboxes)
    p->ECanRegs->CANMC.all = ECanShadow.CANMC.all;

	EDIS;    // This is needed to disable write to EALLOW protected registers

	IER |= M_INT9;

	return p;
}

/* Set port bitrate. */
UNS8 canSetBitrate(CAN_PORT port, unsigned long bitrate)
{
	struct ECAN_REGS shadow;

	Uint16 brp;
	switch (bitrate) {
		case 1000000: brp = 4;  break;
		case  500000: brp = 9;  break;
		case  250000: brp = 19; break;
		case  125000: brp = 39; break;
		case  100000: brp = 49; break;
		case   62500: brp = 79; break;
		case   50000: brp = 99; break;
		default: return 1; // Invalid baud rate, error.
	}

	EALLOW;

	/* Configure bit timing parameters for eCAN*/
	shadow.CANMC.all = port->ECanRegs->CANMC.all;
	shadow.CANMC.bit.CCR = 1 ;            // Set CCR = 1
	port->ECanRegs->CANMC.all = shadow.CANMC.all;

	shadow.CANES.all = port->ECanRegs->CANES.all;
	do {
		shadow.CANES.all = port->ECanRegs->CANES.all;
	} while(shadow.CANES.bit.CCE != 1 );  		// Wait for CCE bit to be set..

	shadow.CANBTC.all = 0;
	shadow.CANBTC.bit.BRPREG   = brp;
	shadow.CANBTC.bit.TSEG1REG = 10;  // Total bit time 15 QT
	shadow.CANBTC.bit.TSEG2REG = 2;   // Sampling point 80%

	shadow.CANBTC.bit.SAM = 1;
	port->ECanRegs->CANBTC.all = shadow.CANBTC.all;

	shadow.CANMC.all = port->ECanRegs->CANMC.all;
	shadow.CANMC.bit.CCR = 0 ;            // Set CCR = 0
	port->ECanRegs->CANMC.all = shadow.CANMC.all;

	shadow.CANES.all = port->ECanRegs->CANES.all;
	do {
		shadow.CANES.all = port->ECanRegs->CANES.all;
	} while(shadow.CANES.bit.CCE != 0 ); 		// Wait for CCE bit to be  cleared..

	EDIS;

	return 1;
}

static unsigned long TranslateBaudRate(char *rate)
{
	if(strcmp(rate, "1M") == 0)   return 1000000;
	if(strcmp(rate, "800k") == 0) return  800000;
	if(strcmp(rate, "500K") == 0) return  500000;
	if(strcmp(rate, "250K") == 0) return  250000;
	if(strcmp(rate, "125K") == 0) return  125000;
	if(strcmp(rate, "100K") == 0) return  100000;
	if(strcmp(rate, "62K") == 0)  return   62500;
	if(strcmp(rate, "50K") == 0)  return   50000;

	return 0;
}

UNS8 canChangeBaudRate(CAN_PORT port, char *rate)
{
	return canSetBitrate(port, TranslateBaudRate(rate));
}

/******************************************************************************
The driver send a CAN message passed from the CANopen stack
INPUT	CAN_PORT p CAN port to use
	Message *m pointer to message to send
OUTPUT	1 if  hardware -> CAN frame
******************************************************************************/
unsigned char canSend(CAN_PORT p, Message *m)
{
	Uint32 mask = p->ECanRegs->CANME.all;

	// Disable interrupts to avoid re-entrancy problems.
	DINT;

	volatile struct MBOX *mbox = &p->ECanMboxes->MBOX0;
	int i;
	for (i = 0; i < CAN_TX_MB; i++, mbox++) {
		if ((mask & (1UL << i)) == 0) {
			// Found a free mailbox.
			mbox->MSGID.all = (Uint32)m->cob_id << 18;
			mbox->MSGCTRL.all = (m->len & 0x0F) | (m->rtr & 0x01) << 4 | (~m->cob_id & 0xF0) << (12 - 8); // Length | RTR | transmission priority
			mbox->MDL.all = m->data[3] | (m->data[2] << 8) | ((Uint32)m->data[1] << 16) | ((Uint32)m->data[0] << 24);
			mbox->MDH.all = m->data[7] | (m->data[6] << 8) | ((Uint32)m->data[5] << 16) | ((Uint32)m->data[4] << 24);

			mask = p->ECanRegs->CANME.all;
			p->ECanRegs->CANME.all = mask | (1UL << i);

			if (!_in_int) EINT;

			// Start transfer.
			p->ECanRegs->CANTRS.all = 1UL << i;

			return 1;
		}
	}

	if (!_in_int) EINT;
	return 0;
}

/******************************************************************************
The driver passes a received CAN message to the stack
INPUT	Message *m pointer to received CAN message
******************************************************************************/
static void canReceive(CAN_PORT p, UNS8 box, Message *m)
{
	volatile struct MBOX *mb = &p->ECanMboxes->MBOX0 + box;

	m->cob_id = mb->MSGID.bit.STDMSGID;
	m->rtr    = mb->MSGCTRL.bit.RTR;
	m->len    = mb->MSGCTRL.bit.DLC;
	union CANMDL_REG low = mb->MDL;
	union CANMDH_REG high = mb->MDH;
	m->data[0] = low.byte.BYTE0;
	m->data[1] = low.byte.BYTE1;
	m->data[2] = low.byte.BYTE2;
	m->data[3] = low.byte.BYTE3;
	m->data[4] = high.byte.BYTE4;
	m->data[5] = high.byte.BYTE5;
	m->data[6] = high.byte.BYTE6;
	m->data[7] = high.byte.BYTE7;

	p->ECanRegs->CANRMP.all = 1UL << box;
}

/******************************************************************************
******************************************************************************/
void canSetMsgFilter(CAN_PORT p, UNS8 nodeId)
{
	DINT;
	EALLOW;  // This is needed to write to EALLOW protected registers.

	// Disable all receive mailboxes.
	Uint32 canme = p->ECanRegs->CANME.all;
	p->ECanRegs->CANME.all = canme & ((1UL << CAN_TX_MB) - 1);

	volatile union CANLAM_REG *lam = &p->ECanLAMRegs->LAM0 + CAN_TX_MB;
	volatile struct MBOX *mbox = &p->ECanMboxes->MBOX0 + CAN_TX_MB;
	int i;
	for (i = 0; i < CAN_RX_MB - CAN_SPECIAL_RX_MB; i++, lam++, mbox++) {
		lam->all = 0x9E000000; // Compare node id, but accept any service.
		mbox->MSGID.all = 0x40000000 | ((UNS32)nodeId << 18);
	}
	// Node guarding
	p->ECanLAMRegs->LAM26.all       = 0x81FFFFFF; // Compare service, but accept any node id.
	p->ECanMboxes->MBOX26.MSGID.all = 0x40000000 | ((UNS32)NODE_GUARD << (18 + 7));
	p->ECanLAMRegs->LAM27.all       = 0x81FFFFFF; // Compare service, but accept any node id.
	p->ECanMboxes->MBOX27.MSGID.all = 0x40000000 | ((UNS32)NODE_GUARD << (18 + 7));
	// Sync / Emergency
	p->ECanLAMRegs->LAM28.all       = 0x81FFFFFF; // Compare service, but accept any node id.
	p->ECanMboxes->MBOX28.MSGID.all = 0x40000000 | ((UNS32)SYNC << (18 + 7));
	p->ECanLAMRegs->LAM29.all       = 0x81FFFFFF; // Compare service, but accept any node id.
	p->ECanMboxes->MBOX29.MSGID.all = 0x40000000 | ((UNS32)SYNC << (18 + 7));
	// Time
	p->ECanLAMRegs->LAM30.all       = 0x81FFFFFF; // Compare service, but accept any node id.
	p->ECanMboxes->MBOX30.MSGID.all = 0x40000000 | ((UNS32)TIME_STAMP << (18 + 7));
	// NMT
	p->ECanLAMRegs->LAM31.all       = 0x81FFFFFF; // Compare service, but accept any node id.
	p->ECanMboxes->MBOX31.MSGID.all = 0x40000000 | ((UNS32)NMT << (18 + 7));

	// Configure overwrite protection for special.
	Uint32 opc = p->ECanRegs->CANOPC.all;
	opc &= ~(0x1DUL << (CAN_MB - CAN_SPECIAL_RX_MB));
	p->ECanRegs->CANOPC.all = opc;

	// Restore active mailboxes.
	p->ECanRegs->CANME.all  = canme;

	EDIS;
	EINT;
}

/******************************************************************************

 ******************************* CAN INTERRUPT  *******************************/

#pragma CODE_SECTION(can_irq_mbox_handler, "ramfuncs");
static void can_irq_mbox_handler(CAN_PORT p)
{
	static Message m = Message_Initializer; // contain a CAN message

	Uint16 status = p->ECanRegs->CANGIF1.all;

	if (status & 0x8000) {
		// Message box interrupt.
		unsigned char mbox = status & 0x1F;

		if (mbox < CAN_TX_MB) {
			// Transmit finished.
			p->ECanRegs->CANTA.all = 1UL << mbox;
			Uint32 me = p->ECanRegs->CANME.all;
			me &= ~(1UL << mbox);
			p->ECanRegs->CANME.all = me;
		} else {
			// Message received.
			_in_int = 1;
			canReceive(p, mbox, &m);
			canDispatch(p->co, &m);
			_in_int = 0;
		}
	}
}

interrupt void can_irq_mbox_handler_a(void)
{
	can_irq_mbox_handler(&_can_ports[0]);
	PieCtrlRegs.PIEACK.all = PIEACK_GROUP9;
}

interrupt void can_irq_mbox_handler_b(void)
{
	can_irq_mbox_handler(&_can_ports[1]);
	PieCtrlRegs.PIEACK.all = PIEACK_GROUP9;
}
