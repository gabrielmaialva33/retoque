int main()
{
    using namespace DlssNr::SlPresent;
    void* table[70] {};
    table[9] = reinterpret_cast<void*>(RealClose);
    table[10] = reinterpret_cast<void*>(RealReset);
    table[68] = reinterpret_cast<void*>(RealBegin);
    table[69] = reinterpret_cast<void*>(RealEnd);
    ID3D12GraphicsCommandList list;
    list.table = table;
    assert(!CopyRecording(nullptr));
    list.type = 2;
    assert(!CopyRecording(&list) && attached == 0);
    list.type = 0;
    for (int failure = 1; failure <= 4; ++failure)
    {
        detourFailure = failure;
        failed = false;
        assert(!CopyRecording(&list) && failed && !installed);
        const auto previous = attached;
        assert(!CopyRecording(&list) && attached == previous);
    }
    detourFailure = 0;
    failed = false;
    assert(!CopyRecording(&list));
    assert(installed && !list.stored);
    assert(Reset(&list, nullptr, nullptr) == 0);
    assert(CopyRecording(&list));
    Begin(&list, 0, nullptr, nullptr, 0);
    assert(!CopyRecording(&list));
    End(&list);
    assert(CopyRecording(&list));
    Begin(&list, 0, nullptr, nullptr, 1);
    End(&list);
    assert(!CopyRecording(&list));
    Reset(&list, nullptr, nullptr);
    assert(CopyRecording(&list));
    Close(&list);
    assert(!CopyRecording(&list));
    resetSuccess = false;
    assert(Reset(&list, nullptr, nullptr) < 0);
    assert(!CopyRecording(&list));
    resetSuccess = true;
    Reset(&list, nullptr, nullptr);
    ID3D12GraphicsCommandList fresh;
    fresh.table = table;
    assert(!CopyRecording(&fresh));
    void* alternate[70];
    std::memcpy(alternate, table, sizeof(table));
    alternate[69] = nullptr;
    list.table = alternate;
    assert(!CopyRecording(&list));
    list.table = table;
    assert(CopyRecording(&list));
    list.failStore = true;
    Begin(&list, 0, nullptr, nullptr, 0);
    assert(!CopyRecording(&list));
    assert(forwards == 10);
    puts("PASS: production recording detours forward and refuse unknown/closed/failed/pass/suspended/incompatible "
         "lists");
}
