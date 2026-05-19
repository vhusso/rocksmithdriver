HRESULT STDMETHODCALLTYPE QueryInterface(void* inDriver, REFIID iid, LPVOID* outInterface) {
    if (outInterface == nullptr) {
        return E_POINTER;
    }
    CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(nullptr, iid);
    if (CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID) || CFEqual(requested, IUnknownUUID)) {
        AddRef(nullptr);
        *outInterface = inDriver;
        CFRelease(requested);
        return S_OK;
    }
    CFRelease(requested);
    *outInterface = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE AddRef(void*) {
    return gRefCount.fetch_add(1) + 1;
}

ULONG STDMETHODCALLTYPE Release(void*) {
    UInt32 old = gRefCount.fetch_sub(1);
    if (old == 1) {
        CFPlugInRemoveInstanceForFactory(factoryUUID());
    }
    return old > 0 ? old - 1 : 0;
}

AudioServerPlugInDriverInterface gDriverInterface = {
    nullptr,
    QueryInterface,
    AddRef,
    Release,
    Initialize,
    CreateDevice,
    DestroyDevice,
    AddRemoveClient,
    AddRemoveClient,
    NoOpConfig,
    NoOpConfig,
    HasProperty,
    IsPropertySettable,
    GetPropertyDataSize,
    GetPropertyData,
    SetPropertyData,
    StartIO,
    StopIO,
    GetZeroTimeStamp,
    WillDoIOOperation,
    BeginEndIO,
    DoIOOperation,
    BeginEndIO
};

AudioServerPlugInDriverInterface* gDriverInterfacePtr = &gDriverInterface;
