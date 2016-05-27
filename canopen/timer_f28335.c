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

// Includes for the Canfestival driver
#include <DSP2833x_Device.h>
#include "canfestival.h"
#include "timer.h"
//#include <PeripheralHeaderIncludes.h>

// Define the timer registers
static TIMEVAL last_time_set = TIMEVAL_MAX;
static volatile TIMEVAL next_alarm = TIMEVAL_MAX;

#pragma CODE_SECTION(timer_can_irq_handler, "ramfuncs");
interrupt void timer_can_irq_handler(void);

void initCanTimer(void)
{
	// Use a different timer if you want to use DSP/BIOS!
	
	ConfigCpuTimer(&CpuTimer2, 150.0f, 1000.0f);

	EALLOW;  // This is needed to write to EALLOW protected registers
	PieVectTable.TINT2 = &timer_can_irq_handler;
	EDIS;    // This is needed to disable write to EALLOW protected registers

	IER |= M_INT14;

	// Start Timer
	StartCpuTimer2();
}

#pragma CODE_SECTION(setTimer, "ramfuncs");
void setTimer(TIMEVAL value)
{
	next_alarm += value;
}

#pragma CODE_SECTION(getElapsedTime, "ramfuncs");
TIMEVAL getElapsedTime(void)
{
	TIMEVAL timer = CpuTimer2.InterruptCount;	// Copy the value of the running timer
	// Calculate the time difference
	return timer - last_time_set;
}


//*----------------------------------------------------------------------------
//* Function Name       : timer_can_irq_handler
//* Object              : C handler interrupt function by the interrupts
//*                       assembling routine
//* Output Parameters   : calls TimeDispatch
//*----------------------------------------------------------------------------
interrupt void timer_can_irq_handler(void)
{
	last_time_set = ++CpuTimer2.InterruptCount;
	if (last_time_set >= next_alarm) {
		_in_int = 1;
		TimeDispatch();
		_in_int = 0;
	}
}
