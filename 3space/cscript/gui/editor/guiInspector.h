#ifndef _GUI_INSPECTOR_SHIM_H_
#define _GUI_INSPECTOR_SHIM_H_

// Open Siege spec 15/02b — shim for upstream gui/editor/guiInspector.h.
//
// `SimObject::onInspect(GuiInspector*)` is the only callsite in the VM path.
// The body calls Con::executef with the inspector pointer, which is
// template-marshalled via `EngineMarshallData<T*>` (engineAPI.h). That
// template invokes `object->getId()`, so a forward declaration alone is
// not enough — we need a stub class that exposes `getId()`.
//
// The full upstream GuiInspector lives in gui/editor/guiInspector.h and is
// a SimObject-derived GUI editor control. We have NOT vendored gui/.
// The script-VM target never instantiates a real GuiInspector — SimObject::
// onInspect is only invoked when an editor is attached, which never happens
// inside cscript_core / cscript-eval.

class GuiInspector
{
public:
    int getId() const { return 0; }
};

#endif // _GUI_INSPECTOR_SHIM_H_
