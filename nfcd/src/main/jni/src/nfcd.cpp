#include <nfcd/nfcd.h>
#include <link.h>
#include <dlfcn.h>

HookGlobals globals;

void hook_nfaConnectionCallback(uint8_t event, void *eventData) {
    auto eventName = System::nfaEventName(event);

    if (eventData)
        LOGI("hook_NFA_Event: %s(%d) with status %d", eventName.c_str(), event, *(uint8_t *)eventData);
    else
        LOGI("hook_NFA_Event: %s(%d)", eventName.c_str(), event);

    // call original callback
    globals.origNfaConnCBack(event, eventData);
    // enqueue event
    globals.eventQueue.enqueue(event, eventData ? *(uint8_t*)eventData : 0);
}

/**
 * Prevent already set values from being overwritten.
 */
tNFC_STATUS hook_NFC_SetConfig(uint8_t tlv_size, uint8_t *p_param_tlvs) {
    globals.hNFC_SetConfig->precall();

    LOGI("NFC_SetConfig()");

    Config cfg, actual;
    cfg.parse(tlv_size, p_param_tlvs);

    for (auto &opt : cfg.options()) {
        // indicates whether this option would override one of the hook options
        bool conflict = false;
        for (auto &hook_opt : globals.hookValues.options())
            if (hook_opt.type() == opt.type())
                conflict = true;

        // log config values with type codes
        std::stringstream bruce;
        bruce << "NFC_SetConfig Option " << opt.name() << "(" << (int)opt.type() << ", "
              << (!globals.guardEnabled ? "own" : "system") << ", "
              << (globals.guardEnabled && conflict ? "blocked" : "pass") << ")";
        loghex(bruce.str().c_str(), opt.value(), opt.len());

        // prevent config values from overriding hook iff guard is enabled
        if (!globals.guardEnabled || !conflict)
            actual.add(opt);
    }

    config_ref bin_stream;
    actual.build(bin_stream);
    tNFC_STATUS r = globals.hNFC_SetConfig->call<def_NFC_SetConfig>(actual.total(), bin_stream.get());

    globals.hNFC_SetConfig->postcall();
    return r;
}

tNFA_STATUS hook_NFA_Enable(void *p_dm_cback, void *p_conn_cback) {
    globals.hNFA_Enable->precall();
    LOGD("hook_NFA_Enable()");

    std::lock_guard<std::mutex> lock(globals.nfaConnCBackMutex);
    LOGD("hook_NFA_Enable: Hooking p_conn_cback");

    globals.origNfaConnCBack = (def_NFA_CONN_CBACK *) p_conn_cback;
    auto result = globals.hNFA_Enable->call<decltype(hook_NFA_Enable)>(p_dm_cback,
        (void *) hook_nfaConnectionCallback);

    LOGD("Delayed hook success");
    globals.hookEnabled = true;

    globals.hNFA_Enable->postcall();
    return result;
}

tNFC_STATUS hook_ce_select_t4t() {
    globals.hce_select_t4t->precall();

    LOGD("hook_ce_select_t4t()");
    LOGD("Patch enabled: %d", globals.patchEnabled);

    tNFC_STATUS r = globals.hce_select_t4t->call<def_ce_select_t4t>();
    if (globals.patchEnabled) {
        int offset = System::sdkInt() < System::O_1 ? CE_CB_STATUS_PRE_O : CE_CB_STATUS_POST_O;
        auto ce_cb_status = globals.hce_cb->address<uint8_t>() + offset;
        // bypass ISO 7816 SELECT requirement for AID selection
        *ce_cb_status |= CE_T4T_STATUS_WILDCARD_AID_SELECTED;
    }

    globals.hce_select_t4t->postcall();
    return r;
}

HookGlobals::HookGlobals() {
    LOG_ASSERT_S(mapInfo.create(), return, "Could not create map");

    // check if NCI library exists and is readable + is loaded
    mLibrary = findLibNFC();
    LOG_ASSERT_S(!mLibrary.empty(), return, "Library not found or not accessible");

    LOGI("Library found at %s", mLibrary.c_str());
    mLibraryRe = "^" + StringUtil::escapeBRE(mLibrary) + "$";

    // create library symbol table
    LOG_ASSERT_S(symbolTable.create(mLibrary), return, "Building symbol table failed");

    // try to obtain handle of already loaded library
    mHandle = dlopen(mLibrary.c_str(), RTLD_NOLOAD);
    LOG_ASSERT_S(mHandle, return, "Could not obtain library handle");

    // begin installing hooks
    IHook::init();
    bool success = false;
    {
        // NFC/NFA main functions
        ASSERT_X(hNFC_SetConfig = hookSymbol("NFC_SetConfig", (void *)&hook_NFC_SetConfig));
        ASSERT_X(hNFA_Enable = hookSymbol("NFA_Enable", (void *)&hook_NFA_Enable));

        // discovery
        ASSERT_X(hNFA_StartRfDiscovery = lookupSymbol("NFA_StartRfDiscovery"));
        ASSERT_X(hNFA_StopRfDiscovery = lookupSymbol("NFA_StopRfDiscovery"));

        // polling / listening
        ASSERT_X(hNFA_EnablePolling = lookupSymbol("NFA_EnablePolling"));
        ASSERT_X(hNFA_DisablePolling = lookupSymbol("NFA_DisablePolling"));
        ASSERT_X(hNFA_SetP2pListenTech = lookupSymbol("NFA_SetP2pListenTech"));

        // NFC routing
        ASSERT_X(hce_select_t4t = hookSymbol("ce_select_t4t", (void *)&hook_ce_select_t4t));
        ASSERT_X(hce_cb = lookupSymbol("ce_cb"));

        // NFA callback
        ASSERT_X(nfa_dm_cb = lookupSymbol("nfa_dm_cb"));
        if (!(success = hookNFACB()))
            LOGW("Hooking NFA_CB failed, hook may be delayed (waiting for NFA_Enable)");
    }
    // finish installing hooks
    LOG_ASSERT_S(IHook::finish(), return, "Hooking failed");

    // hooking success
    hookEnabled = success;
}

std::string HookGlobals::findLibNFC() const {
    for (const auto &candidate : mapInfo.loadedLibraries()) {
        // library path must contain "nfc" somewhere
        if (!StringUtil::strContains(candidate, "nfc"))
            continue;

        // library file must be accessible
        if (access(candidate.c_str(), R_OK) != 0)
            continue;

        // library symbol table must contain the expected symbol
        SymbolTable table;
        if (table.create(candidate) && table.contains("NFC_SetConfig"))
            return candidate;
    }

    return "";
}

bool HookGlobals::checkNFACBOffset(uint32_t offset) {
    LOGD("checkOffset: trying offset 0x%x", offset);

    // try to get nfa_dm_cb[offset]
    auto **p_nfa_conn_cback = (def_NFA_CONN_CBACK**)(nfa_dm_cb->address<uint8_t>() + offset);
    LOG_ASSERT_S(*p_nfa_conn_cback, return false, "p_conn_cback is null, offset likely invalid");

    auto rangeInfo = mapInfo.rangeFromAddress(reinterpret_cast<uintptr_t>(*p_nfa_conn_cback));
    LOG_ASSERT_S(rangeInfo, return false, "p_conn_cback range info invalid");
    LOGD("checkOffset: candidate p_conn_cback %p with permissions %d in object file %s",
         *p_nfa_conn_cback, rangeInfo->perms, rangeInfo->label.c_str());
    LOG_ASSERT_S((rangeInfo->perms & 1) == 1, return false,
                 "p_conn_cback permissions not execute, offset likely invalid");
    LOG_ASSERT_S(rangeInfo->label.find("jni") != std::string::npos, return false,
                 "p_conn_cback not in JNI object, offset likely invalid");

    LOGD("checkOffset: success");
    return true;
}

uint32_t HookGlobals::findNFACBOffset() {
    // search [standard_offset, standard_offset + 2]
    for (uint32_t i = 0; i < 2; i++) {
        uint32_t offset = NFA_DM_CB_CONN_CBACK + (i * sizeof(void*));

        if (checkNFACBOffset(offset))
            return offset;
    }

    return 0;
}

bool HookGlobals::hookNFACB() {
    std::lock_guard<std::mutex> lock(nfaConnCBackMutex);

    uint32_t offset = findNFACBOffset();
    LOG_ASSERT_S(offset != 0, return false, "Finding p_conn_cback offset failed");

    auto **p_nfa_conn_cback = (def_NFA_CONN_CBACK**)(nfa_dm_cb->address<uint8_t>() + offset);
    LOG_ASSERT_S(*p_nfa_conn_cback, return false, "NFA_CB is null");

    // ensure to hook only once
    if (*p_nfa_conn_cback != &hook_nfaConnectionCallback) {
        LOGD("hookNFACB: Hooking NFA_CB");

        // save old nfa connection callback
        origNfaConnCBack = *p_nfa_conn_cback;
        // set new nfa connection callback
        *p_nfa_conn_cback = &hook_nfaConnectionCallback;
    }
    else
        LOGD("hookNFACB: NFA_CB already hooked");

    return true;
}

Symbol_ref HookGlobals::lookupSymbol(const std::string &name) const {
    Symbol_ref result(new Symbol(name, mHandle));
    LOG_ASSERT_S(result, return nullptr, "Symbol lookup failed for %s", name.c_str());
    return result;
}

IHook_ref HookGlobals::hookSymbol(const std::string &name, void *hook) const {
    auto result = IHook::hook(name, hook, mHandle, mLibraryRe);
    LOG_ASSERT_S(result, return nullptr, "Hooking failed for %s", name.c_str());
    return result;
}
