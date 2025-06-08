#include "internal.h"

VOID
RingBufferInitialize(
    _In_  PRING_BUFFER      Self,
    _In_reads_bytes_(BufferSize)
    BYTE* Buffer,
    _In_  size_t            BufferSize
)
{
    Self->Size = BufferSize;
    Self->Base = Buffer;
    Self->End = Buffer + BufferSize;
    Self->Head = Buffer;
    Self->Tail = Buffer;
}


VOID
RingBufferGetAvailableSpace(
    _In_  PRING_BUFFER      Self,
    _Out_ size_t* AvailableSpace
)
{
    BYTE* headSnapshot = NULL;
    BYTE* tailSnapshot = NULL;
    BYTE* tailPlusOne = NULL;

    ASSERT(AvailableSpace);

    //
    // Take a snapshot of the head and tail pointers. We will compute the
    // available space based on this snapshot. This is safe to do in a
    // single-producer, single-consumer model, because -
    //     * A producer will call GetAvailableSpace() to determine whether
    //       there is enough space to write the data it is trying to write.
    //       The only other thread that could modify the amount of space
    //       available is the consumer thread, which can only increase the
    //       amount of space available. Hence it is safe for the producer
    //       to write based on this snapshot.
    //     * A consumer thread will call GetAvailableSpace() to determine
    //       whether there is enough data in the buffer for it to read.
    //       (Available data = Buffer size - Available space). The only
    //       other thread that could modify the amount of space available
    //       is the producer thread, which can only decrease the amount of
    //       space available (thereby increasing the amount of data
    //       available. Hence it is safe for the consumer to read based on
    //       this snapshot.
    //
    headSnapshot = Self->Head;
    tailSnapshot = Self->Tail;

    //
    // In order to distinguish between a full buffer and an empty buffer,
    // we always leave the last byte of the buffer unused. So, an empty
    // buffer is denoted by -
    //      tail == head
    // ... and a full buffer is denoted by -
    //      (tail+1) == head
    //
    tailPlusOne = ((tailSnapshot + 1) == Self->End) ? Self->Base : (tailSnapshot + 1);

    if (tailPlusOne == headSnapshot)
    {
        *AvailableSpace = 0;
    }
    else if (tailSnapshot == headSnapshot)
    {
        *AvailableSpace = Self->Size - 1;
    }
    else
    {
        if (tailSnapshot > headSnapshot)
        {
            *AvailableSpace = Self->Size - (tailSnapshot - headSnapshot) - 1;
        }
        else
        {
            *AvailableSpace = (headSnapshot - tailSnapshot) - 1;
        }
    }
}


VOID
RingBufferGetAvailableData(
    _In_  PRING_BUFFER      Self,
    _Out_ size_t* AvailableData
)
{
    size_t                  availableSpace;

    ASSERT(AvailableData);

    RingBufferGetAvailableSpace(Self, &availableSpace);

    *AvailableData = Self->Size - availableSpace - 1;
}


NTSTATUS
RingBufferWrite(
    _In_  PRING_BUFFER      Self,
    _In_reads_bytes_(DataSize)
    BYTE* Data,
    _In_  size_t            DataSize
)
{
    size_t                  availableSpace;
    size_t                  bytesToCopy;
    size_t                  spaceFromCurrToEnd;

    ASSERT(Data && (0 != DataSize));

    if (Self->Tail >= Self->End)
    {
        return STATUS_INTERNAL_ERROR;
    }

    RingBufferGetAvailableSpace(Self, &availableSpace);

    if (availableSpace < DataSize)
    {
        // throw away eccess data
        bytesToCopy = availableSpace;
    }
    else
    {
        bytesToCopy = DataSize;
    }

    if (bytesToCopy)
    {
        if ((Self->Tail + bytesToCopy) > Self->End)
        {
            //
            // The data being written will wrap around the end of the buffer.
            // So the copy has to be done in two steps -
            // * X bytes from current position to end of the buffer
            // * the remaining (bytesToCopy - X) from the start of the buffer
            //

            //
            // The first step of the copy ...
            //
            spaceFromCurrToEnd = Self->End - Self->Tail;

            RtlCopyMemory(Self->Tail, Data, spaceFromCurrToEnd);

            Data += spaceFromCurrToEnd;

            bytesToCopy -= spaceFromCurrToEnd;

            //
            // The second step of the copy ...
            //
            RtlCopyMemory(Self->Base, Data, bytesToCopy);

            Self->Tail = Self->Base + bytesToCopy;
        }
        else
        {
            //
            // Data does NOT wrap around the end of the buffer. Just copy it
            // over in a single step
            //
            RtlCopyMemory(Self->Tail, Data, bytesToCopy);

            Self->Tail += bytesToCopy;
            if (Self->Tail == Self->End)
            {
                //
                // We have exactly reached the end of the buffer. The next
                // write should wrap around and start from the beginning.
                //
                Self->Tail = Self->Base;
            }
        }

        ASSERT(Self->Tail < Self->End);
    }

    return STATUS_SUCCESS;
}


NTSTATUS
RingBufferRead(
    _In_  PRING_BUFFER      Self,
    _Out_writes_bytes_to_(DataSize, *BytesCopied)
    BYTE* Data,
    _In_  size_t            DataSize,
    _Out_ size_t* BytesCopied
)
{
    size_t                  availableData;
    size_t                  dataFromCurrToEnd;

    ASSERT(Data && (DataSize != 0));

    if (Self->Head >= Self->End)
    {
        return STATUS_INTERNAL_ERROR;
    }

    RingBufferGetAvailableData(Self, &availableData);

    if (availableData == 0)
    {
        *BytesCopied = 0;
        return STATUS_SUCCESS;
    }

    if (DataSize > availableData)
    {
        DataSize = availableData;
    }

    *BytesCopied = DataSize;

    if ((Self->Head + DataSize) > Self->End)
    {
        //
        // The data requested by the caller is wrapped around the end of the
        // buffer. So we'll do the copy in two steps -
        //    * Copy X bytes from the current position to the end buffer into
        //      the caller's buffer
        //    * Copy (DataSize - X) bytes from the beginning to the buffer into
        //      the caller's buffer
        //

        //
        // The first step of the copy ...
        //
        dataFromCurrToEnd = Self->End - Self->Head;
        RtlCopyMemory(Data, Self->Head, dataFromCurrToEnd);
        Data += dataFromCurrToEnd;
        DataSize -= dataFromCurrToEnd;

        //
        // The second step of the copy ...
        //
        RtlCopyMemory(Data, Self->Base, DataSize);

        Self->Head = Self->Base + DataSize;
    }
    else
    {
        //
        // The data in the buffer is NOT wrapped around the end of the buffer.
        // Simply copy the data over to the caller's buffer in a single step.
        //
        RtlCopyMemory(Data, Self->Head, DataSize);

        Self->Head += DataSize;
        if (Self->Head == Self->End)
        {
            //
            // We have exactly reached the end of the buffer. The next
            // read should wrap around and start from the beginning.
            //
            Self->Head = Self->Base;
        }
    }

    ASSERT(Self->Head < Self->End);

    return STATUS_SUCCESS;
}
