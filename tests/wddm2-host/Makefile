# maliwddm's WDDM 2.x answers and one-shot fallback (drivers/maliwddm/wddm2plan.h)
# on the host, plus source checks that the driver takes them from there. `make run`.
CFLAGS ?= -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
CFLAGS += -std=gnu11 -Wall -Wextra -Werror
SRC = ../../drivers/maliwddm

wddm2-host: test.c $(SRC)/wddm2plan.h
	$(CC) $(CFLAGS) -o $@ test.c

# What the first 2.0 probe got wrong must not come back: our own context as
# dxgkrnl's physical adapter handle, a constant paging node, and the
# segment's count pass decided by the pointer.
lint:
	@! grep -n 'DxgkPhysicalAdapterHandle *= *(HANDLE)' $(SRC)/*.c || { echo "FAIL: PHYSICALADAPTERCAPS hands back our own context"; exit 1; }
	@grep -q 'DxgkPhysicalAdapterHandle = A->Dxgk.DeviceHandle' $(SRC)/wddm2x.c || { echo "FAIL: PHYSICALADAPTERCAPS does not hand back dxgkrnl's DeviceHandle"; exit 1; }
	@grep -q 'PagingNodeIndex = (WORD)p->PagingNode' $(SRC)/wddm2x.c || { echo "FAIL: PagingNodeIndex not from MwPlan"; exit 1; }
	@grep -q 'Mw2SegmentPass(out->NbSegment' $(SRC)/memory.c || { echo "FAIL: QUERYSEGMENT4 pass not from Mw2SegmentPass"; exit 1; }
	@grep -q 'Mw2BootDecide(&in, &out)' $(SRC)/version.c || { echo "FAIL: the fallback is not wddm2plan.h's"; exit 1; }
	@grep -q 'MwVersionConfirmCheck(A->Renders' $(SRC)/probe.c || { echo "FAIL: the confirm is not the watchdog's"; exit 1; }
	@grep -q 'Mw2PerNodeEntries(Info->OutputDataSize' $(SRC)/adapter.c || { echo "FAIL: QAI 10 not answered per node"; exit 1; }
	@grep -q 'if (MwPlan.PowerDdis) {' $(SRC)/adapter.c || { echo "FAIL: CalibrateGpuClock/SetStablePowerState not registered from MwPlan"; exit 1; }
	@grep -q 'init.DxgkDdiCalibrateGpuClock = T_CalibrateGpuClock' $(SRC)/adapter.c || { echo "FAIL: CalibrateGpuClock not registered"; exit 1; }
	@grep -q 'init.DxgkDdiSetStablePowerState = T_SetStablePowerState' $(SRC)/adapter.c || { echo "FAIL: SetStablePowerState not registered"; exit 1; }
	@grep -q 'MwAdapterStop(dev)' $(SRC)/display.c || { echo "FAIL: StopDeviceAndReleasePostDisplayOwnership does not stop the adapter"; exit 1; }
	@grep -q 'MwCommandStop(a);' $(SRC)/adapter.c || { echo "FAIL: RemoveDevice does not stop the copy thread"; exit 1; }
	@grep -q 'Mw2xRotationSupport(&upd.ContentTransformation.RotationSupport)' $(SRC)/display.c || { echo "FAIL: display.c paths without Offset0 at 2.x"; exit 1; }
	@grep -q 'Mw2xRotationSupport(&upd.ContentTransformation.RotationSupport)' $(SRC)/hpd.c || { echo "FAIL: hpd.c paths without Offset0 at 2.x"; exit 1; }
	@! grep -n 'DxgkCbGetHandleData(' $(SRC)/*.c | grep -v 'MwAllocFromHandle\|return (MW_ALLOCATION \*)A->Dxgk.DxgkCbGetHandleData' || { echo "FAIL: DxgkCbGetHandleData called outside MwAllocFromHandle"; exit 1; }
	@grep -q 'Mw2BudgetMode(MwWddm2Budget, MwWddm2LocalBudget)' $(SRC)/version.c || { echo "FAIL: the budget group is not Mw2BudgetMode's"; exit 1; }
	@grep -q 'd->Flags.LocalBudgetGroup = p->SegLocalBudget;' $(SRC)/memory.c || { echo "FAIL: QUERYSEGMENT4 local budget group not from MwPlan"; exit 1; }
	@! sed -n '/^NTSTATUS MwQuerySegment(/,/^}/p' $(SRC)/memory.c | grep -n 'BudgetGroup\|ApplicationTarget' || { echo "FAIL: the 1.3 segment (QUERYSEGMENT3) gained a budget flag"; exit 1; }
	@grep -q 'in.Default = MW2_WDDM_DEFAULT;' $(SRC)/version.c || { echo "FAIL: version.c does not use the default version"; exit 1; }
	@grep -q 'level = FindLevel(Mw2Wanted(in.Want, in.Default));' $(SRC)/version.c || { echo "FAIL: the level is not the wanted version's"; exit 1; }
	@grep -q 'ParamWrite(L"WddmProbeFellBackBuild", (ULONG)MW_BUILD);' $(SRC)/version.c || { echo "FAIL: a fallback does not record its build (no sticky fallback)"; exit 1; }
	@grep -q 'in.FellBackBuild = ParamRead(L"WddmProbeFellBackBuild"' $(SRC)/version.c || { echo "FAIL: the sticky fallback is not read"; exit 1; }
	@grep -q 'Mw2JournalMax(in.Want' $(SRC)/version.c || { echo "FAIL: the journal is not capped by Mw2JournalMax"; exit 1; }
	@grep -q 'define MW_BC_SLOTS         MW2_JOURNAL_BOOTS' $(SRC)/probe.c || { echo "FAIL: journal boots not MW2_JOURNAL_BOOTS"; exit 1; }
	@echo "lint: ok"

run: wddm2-host lint
	./wddm2-host

clean:
	rm -f wddm2-host
