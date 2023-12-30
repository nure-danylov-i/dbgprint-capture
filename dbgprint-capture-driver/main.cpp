#include <ntddk.h>

#if DBG
#define DebugPrint(x) DbgPrint x
#else
#define DebugPrint(x)
#endif

#define THE_BUFFER_LENGTH 512

#define FILE_DEVICE_IOCTL  0x00008301
#define IOCTL_REFERENCE_EVENT    CTL_CODE(FILE_DEVICE_IOCTL, 0x800, METHOD_NEITHER, FILE_ANY_ACCESS)
#define IOCTL_DEREFERENCE_EVENT  CTL_CODE(FILE_DEVICE_IOCTL, 0x801, METHOD_NEITHER, FILE_ANY_ACCESS)

PVOID gpEventObject = NULL;

PCWSTR gDevName = L"\\Device\\DevDbgPrintCapture";
PCWSTR gLinkName = L"\\DosDevices\\LinkDbgPrintCapture";
PDEVICE_OBJECT gDevObject = NULL;

typedef struct
{
	unsigned char buffer[THE_BUFFER_LENGTH];
	LARGE_INTEGER timestamp;
	unsigned int  number;
} MY_BUFFER, * PMY_BUFFER;

PMY_BUFFER g_buffer;

EXTERN_C _IRQL_requires_max_(SYNCH_LEVEL)
VOID DebugPrintCallback(_In_ PSTRING string, _In_ ULONG componentId, _In_ ULONG level)
{
	UNREFERENCED_PARAMETER(level);
	UNREFERENCED_PARAMETER(componentId);

	if (gpEventObject == NULL)
	{
		return;
	}

	KIRQL oldLevel = KeRaiseIrqlToSynchLevel();

	KeQuerySystemTimePrecise(&g_buffer->timestamp);
	g_buffer->number++;

	RtlZeroMemory(g_buffer->buffer, THE_BUFFER_LENGTH);
	RtlMoveMemory(g_buffer->buffer, string->Buffer, string->Length);

	KeSetEvent((PRKEVENT)gpEventObject, 0, FALSE);
	KeLowerIrql(oldLevel);
	return;
}

NTSTATUS OnCreate(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	UNREFERENCED_PARAMETER(DeviceObject);
	Irp->IoStatus.Status = STATUS_SUCCESS;
	Irp->IoStatus.Information = 0;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return STATUS_SUCCESS;
}

NTSTATUS OnClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	UNREFERENCED_PARAMETER(DeviceObject);
	Irp->IoStatus.Status = STATUS_SUCCESS;
	Irp->IoStatus.Information = 0;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return STATUS_SUCCESS;
}

NTSTATUS OnControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	UNREFERENCED_PARAMETER(DeviceObject);
	NTSTATUS status = STATUS_SUCCESS;
	PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
	HANDLE hEvent = NULL;

	switch (stack->Parameters.DeviceIoControl.IoControlCode)
	{
	case IOCTL_REFERENCE_EVENT:
		hEvent = stack->Parameters.DeviceIoControl.Type3InputBuffer;
		status = ObReferenceObjectByHandle(
			hEvent,
			GENERIC_ALL,
			NULL,
			KernelMode,
			&gpEventObject,
			NULL);
		break;
	case IOCTL_DEREFERENCE_EVENT:
		if (gpEventObject)
			ObDereferenceObject(gpEventObject);
		break;
	default:
		break;
	}

	Irp->IoStatus.Status = STATUS_SUCCESS;
	Irp->IoStatus.Information = 0;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);

	return status;
}

NTSTATUS OnRead(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	UNREFERENCED_PARAMETER(DeviceObject);
	PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
	ULONG bufsize = 0;
	PVOID buf = NULL;
	
	buf = Irp->UserBuffer;
	
	bufsize = stack->Parameters.Read.Length;
	RtlMoveMemory(
		buf,
		g_buffer,
		bufsize > sizeof(MY_BUFFER) ? sizeof(MY_BUFFER) : bufsize
	);
	Irp->IoStatus.Information = bufsize > sizeof(MY_BUFFER) ?
	sizeof(MY_BUFFER) : bufsize;
	
	Irp->IoStatus.Status = STATUS_SUCCESS;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);

	return STATUS_SUCCESS;
}

void OnDrvUnload(_In_ PDRIVER_OBJECT DriverObject)
{
	UNICODE_STRING uLinkName;

	DbgSetDebugPrintCallback(DebugPrintCallback, FALSE);

	RtlInitUnicodeString(&uLinkName, gLinkName);
	IoDeleteSymbolicLink(&uLinkName);

	PDEVICE_OBJECT next;
	next = DriverObject->DeviceObject;
	while (next != NULL)
	{
		PDEVICE_OBJECT device = next;
		next = next->NextDevice;
		IoDeleteDevice(device);
	}
	ExFreePool(g_buffer);
}

NTSTATUS CreateDeviceLinkPair(PDRIVER_OBJECT driverObject, PDEVICE_OBJECT deviceObject, ULONG flags,
	PCWSTR devName, PCWSTR linkName)
{
	UNICODE_STRING uDevName;
	RtlInitUnicodeString(&uDevName, devName);

	UNICODE_STRING uLinkName;
	RtlInitUnicodeString(&uLinkName, linkName);

	NTSTATUS status = IoCreateDevice(
		driverObject,
		sizeof(MY_BUFFER),
		&uDevName,
		32769,
		0,
		FALSE,
		&deviceObject);

	if (!NT_SUCCESS(status))
	{
		DebugPrint(("dbgprint-caputre: IoCreateDevice failed: error 0x%08X\n", status));
		return status;
	}
	KdPrint(("dbgprint-caputre: IoCreateDevice succeded\n", devName));

	deviceObject->Flags |= flags;

	status = IoCreateSymbolicLink(&uLinkName, &uDevName);

	if (!NT_SUCCESS(status))
	{
		IoDeleteDevice(deviceObject);
		DebugPrint(("dbgprint-caputre: IoCreateSymbolicLink failed: error 0x%08X\n", status));
		return status;
	}

	return STATUS_SUCCESS;
}

extern "C"
NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject,
	_In_ PUNICODE_STRING RegistryPath) {
	UNREFERENCED_PARAMETER(RegistryPath);
	DriverObject->DriverUnload = OnDrvUnload;
	DebugPrint(("dbgprint-caputre: DriverEntry called\n"));

	CreateDeviceLinkPair(DriverObject, gDevObject, NULL, gDevName, gLinkName);
	
	DriverObject->MajorFunction[IRP_MJ_CREATE] = OnCreate;
	DriverObject->MajorFunction[IRP_MJ_CLOSE] = OnClose;
	DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = OnControl;
	DriverObject->MajorFunction[IRP_MJ_READ] = OnRead;

	g_buffer = (PMY_BUFFER)ExAllocatePool2(
		POOL_FLAG_NON_PAGED, sizeof(MY_BUFFER), 'mytg');

	DbgSetDebugPrintCallback(DebugPrintCallback, TRUE);

	return STATUS_SUCCESS;
}