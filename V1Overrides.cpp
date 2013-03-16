//
//  V1Overrides.cpp
//  GenericUSBXHCI
//
//  Created by Zenith432 on December 26th, 2012.
//  Copyright (c) 2012-2013 Zenith432. All rights reserved.
//

#include "GenericUSBXHCI.h"
#include "Async.h"
#include "XHCITypes.h"
#include <IOKit/IOTimerEventSource.h>

#define CLASS GenericUSBXHCI
#define super IOUSBControllerV3

#pragma mark -
#pragma mark IOUSBController Overrides
#pragma mark -

__attribute__((noinline))
UInt32 CLASS::GetErrataBits(UInt16 vendorID, UInt16 deviceID, UInt16 revisionID)
{
	ErrataListEntry const errataList[] = {
		{ 0x1033U, 0x0194U, 0U, UINT16_MAX, kErrataRenesas },	// Renesas uPD720200
		{ 0x1B73U, 0x1000U, 0U, UINT16_MAX, kErrataDisableMSI },	// Fresco Logic FL1000
		{ 0x8086U, 0x1E31U, 0U, UINT16_MAX,
			kErrataAllowControllerDoze |
			kErrataParkRing | kErrataIntelPCIRoutingExtension |
			kErrataDisableComplianceExtension | kErrataIntelPantherPoint},	// Intel Series 7/C210
		{ 0x1B21U, 0U, 0U, UINT16_MAX, kErrataASMedia },	// Any ASMedia
		{ 0x1B73U, 0U, 0U, UINT16_MAX, kErrataFrescoLogic },	// Any Fresco Logic (FL1000, FL1009, FL1100)
		{ 0x1B73U, 0x1100U, 0U, 0x10U, kErrataFL1100 }	// Fresco Logic FL1100
	};
	ErrataListEntry const* entryPtr;
	uint32_t i, errata = 0U;
	for (i = 0U, entryPtr = &errataList[0]; i < (sizeof(errataList) / sizeof(errataList[0])); ++i, ++entryPtr)
		if (vendorID == entryPtr->vendID &&
			(deviceID == entryPtr->deviceID || !entryPtr->deviceID) &&
			revisionID >= entryPtr->revisionLo &&
			revisionID <= entryPtr->revisionHi)
			errata |= entryPtr->errata;
#if __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ >= 1072
	if (getProperty("IOPCITunnelled", gIOServicePlane) == kOSBooleanTrue) {
		_v3ExpansionData->_onThunderbolt = true;
		requireMaxBusStall(25000U);
	}
#endif
	if (gux_options & GUX_OPTION_NO_INTEL_IDLE)
		errata &= ~kErrataAllowControllerDoze;
	if (gux_options & GUX_OPTION_NO_MSI)
		errata |= kErrataDisableMSI;
	return errata;
}

void CLASS::UIMCheckForTimeouts(void)
{
	uint32_t frameNumber, sts, mfIndex;
	uint8_t slot;

	frameNumber = GetFrameNumber32();
	sts = Read32Reg(&_pXHCIOperationalRegisters->USBSts);
	if (m_invalid_regspace) {
		for (slot = 1U; slot <= _numSlots; ++slot)
			CheckSlotForTimeouts(slot, frameNumber);
		if (_expansionData) {
			_watchdogTimerActive = false;
			if (_watchdogUSBTimer)
				_watchdogUSBTimer->cancelTimeout();
		}
		return;
	}
	if ((sts & XHCI_STS_HSE) && !_HSEDetected) {
		IOLog("%s: HSE bit set:%x (1)\n", __FUNCTION__, sts);
		_HSEDetected = true;
	}
	for (slot = 1U; slot <= _numSlots; ++slot)
		if (!IsStillConnectedAndEnabled(slot))
			CheckSlotForTimeouts(slot, frameNumber);
	if (_powerStateChangingTo != -1 && _powerStateChangingTo <= kUSBPowerStateLowPower)
		return;
	mfIndex = Read32Reg(&_pXHCIRuntimeRegisters->MFIndex);
	if (m_invalid_regspace)
		return;
	mfIndex &= XHCI_MFINDEX_MASK;
	if (!mfIndex)
		return;
	for (slot = 1U; slot <= _numSlots; ++slot)
		CheckSlotForTimeouts(slot, frameNumber);
}

IOReturn CLASS::UIMCreateControlTransfer(short functionNumber, short endpointNumber, IOUSBCommand* command,
										 IOMemoryDescriptor* CBP, bool /* bufferRounding */, UInt32 bufferSize,
										 short direction)
{
	IOReturn rc;
	uint32_t mystery;	// Note: structure as fourth uint32_t of Transfer TRB
	uint8_t slot, immediateDataSize;
	ringStruct* pRing;
	ContextStruct* pContext;
	SetupStageHeader smallbuf1;
	SetupStageHeader smallbuf2;

	slot = GetSlotID(functionNumber);
	if (!slot)
		return kIOUSBEndpointNotFound;
	if (endpointNumber)
		return kIOReturnBadArgument;
	pRing = GetRing(slot, 1, 0U);
	if (pRing->isInactive())
		return kIOReturnBadArgument;
	if (GetIntelFlag(slot))
		return AddDummyCommand(pRing, command);
	if (pRing->deleteInProgress)
		return kIOReturnNoDevice;
	XHCIAsyncEndpoint* pEp = pRing->asyncEndpoint;
	if (!pEp)
		return kIOUSBEndpointNotFound;
	if (pEp->unusable)
		return kIOReturnNotPermitted;
	if (CBP && bufferSize) {
		IODMACommand* dmac = command->GetDMACommand();
		if (!dmac) {
			IOLog("%s: no dmaCommand\n", __FUNCTION__);
			return kIOReturnNoMemory;
		}
		IOMemoryDescriptor const* dmac_md = dmac->getMemoryDescriptor();
		if (dmac_md != CBP) {
			IOLog("%s: mismatched CBP (%p) and dmaCommand memory descriptor (%p)\n", __FUNCTION__,
				  CBP, dmac_md);
			return kIOReturnInternalError;
		}
	}
	bzero(&smallbuf1, sizeof smallbuf1);
	if (direction == kUSBNone) {
		if (bufferSize != sizeof smallbuf2)
			return kIOReturnBadArgument;
		if (CBP->readBytes(0U, &smallbuf2, sizeof smallbuf2) != sizeof smallbuf2)
			return kIOReturnInternalError;
		if (smallbuf2.bmRequestType == 0U && smallbuf2.bRequest == 5U) { /* kSetAddress */
			uint16_t deviceAddress = smallbuf2.wValue;
			pContext = GetSlotContext(slot, 1);
			uint16_t maxPacketSize = static_cast<uint16_t>(XHCI_EPCTX_1_MAXP_SIZE_GET(pContext->_e.dwEpCtx1));
			pContext = GetSlotContext(slot);
			_deviceZero.isBeingAddressed = true;
			rc = AddressDevice(slot,
							   maxPacketSize,
							   true,
							   GetSlCtxSpeed(pContext),
							   XHCI_SCTX_2_TT_HUB_SID_GET(pContext->_s.dwSctx2),
							   XHCI_SCTX_2_TT_PORT_NUM_GET(pContext->_s.dwSctx2));
			if (rc != kIOReturnSuccess)
				return rc;
			_addressMapper.HubAddress[deviceAddress] = static_cast<uint8_t>(_deviceZero.HubAddress);
			_addressMapper.PortOnHub[deviceAddress] = static_cast<uint8_t>(_deviceZero.PortOnHub);
			_addressMapper.Slot[deviceAddress] = static_cast<uint8_t>(slot);
			_addressMapper.Active[deviceAddress] = true;
			_deviceZero.HubAddress = 0U;
			_deviceZero.PortOnHub = 0U;
			mystery = XHCI_TRB_3_TYPE_SET(XHCI_TRB_TYPE_NOOP) | XHCI_TRB_3_IOC_BIT;
			immediateDataSize = 0U;
		} else {
			mystery = XHCI_TRB_3_TYPE_SET(XHCI_TRB_TYPE_SETUP_STAGE) | XHCI_TRB_3_IDT_BIT | XHCI_TRB_3_IOC_BIT;
			/*
			 * Set TRT field
			 */
			if (smallbuf2.wLength >= 1U)
				/*
				 * Upper bit of bmRequestType is transfer direction (1 - in, 0 - out)
				 */
				mystery |= (smallbuf2.bmRequestType & 0x80U) ? XHCI_TRB_3_TRT_IN : XHCI_TRB_3_TRT_OUT;
			// else XHCI_TRB_3_TRT_NONE
			bcopy(&smallbuf2, &smallbuf1, sizeof smallbuf1);
			immediateDataSize = sizeof smallbuf1;
		}
	} else if (bufferSize) {
		mystery = XHCI_TRB_3_TYPE_SET(XHCI_TRB_TYPE_DATA_STAGE);
		if (direction == kUSBIn)
			mystery |= XHCI_TRB_3_DIR_IN;
		immediateDataSize = 0xFFU;	// means data is not immediate
	} else if (_deviceZero.isBeingAddressed) {
		_addressMapper.Slot[0] = 0U;
		_addressMapper.Active[0] = false;
		_deviceZero.isBeingAddressed = false;
		mystery = XHCI_TRB_3_TYPE_SET(XHCI_TRB_TYPE_NOOP) | XHCI_TRB_3_IOC_BIT;
		immediateDataSize = 0U;
	} else {
		mystery = XHCI_TRB_3_TYPE_SET(XHCI_TRB_TYPE_STATUS_STAGE) | XHCI_TRB_3_IOC_BIT;
		if (direction == kUSBIn)
			mystery |= XHCI_TRB_3_DIR_IN;
		immediateDataSize = 0U;
	}
	rc = pEp->CreateTDs(command, 0U, mystery, immediateDataSize, reinterpret_cast<uint8_t*>(&smallbuf1));
	pEp->ScheduleTDs();
	return rc;
}

IOReturn CLASS::UIMCreateBulkTransfer(IOUSBCommand* command)
{
	if (!command)
		return kIOReturnBadArgument;
	return CreateTransfer(command, command->GetStreamID());
}

IOReturn CLASS::UIMCreateInterruptTransfer(IOUSBCommand* command)
{
	IOUSBCompletion comp;
	IOMemoryDescriptor* md;
	USBDeviceAddress addr;
	IODMACommand* dmac;

	if (!command)
		return kIOReturnBadArgument;
	comp = command->GetUSLCompletion();
	md = command->GetBuffer();
	if (!md)
		return kIOReturnInternalError;
	addr = command->GetAddress();
	if (addr == _hub3Address || addr == _hub2Address) {
		dmac = command->GetDMACommand();
		if (dmac && dmac->getMemoryDescriptor())
				dmac->clearMemoryDescriptor();
		if (command->GetEndpoint() == 1U) {
			md->setTag(static_cast<uint32_t>(((addr << kUSBAddress_Shift) & kUSBAddress_Mask) |
											 (addr == _hub3Address ? kUSBDeviceSpeedSuper : kUSBDeviceSpeedHigh)));
			return RootHubQueueInterruptRead(md, static_cast<uint32_t>(command->GetReqCount()), comp);
		} else {
			Complete(comp, kIOUSBEndpointNotFound, static_cast<uint32_t>(command->GetReqCount()));
			return kIOUSBEndpointNotFound;
		}
	}
	return CreateTransfer(command, 0U);
}

IOReturn CLASS::UIMCreateIsochTransfer(IOUSBIsocCommand* command)
{
	/*
	 * TBD
	 */
	IOLog("%s\n", __FUNCTION__);
	return super::UIMCreateIsochTransfer(command);
}