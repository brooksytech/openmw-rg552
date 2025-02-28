#include "window.hpp"

#include <MyGUI_InputManager.h>

namespace LuaUi
{
    LuaWindow::LuaWindow()
        : mCaption()
        , mPreviousMouse()
        , mChangeScale()
        , mMoveResize()
    {}

    void LuaWindow::updateTemplate()
    {
        for (auto& [w, _] : mActionWidgets)
        {
            w->eventMouseButtonPressed.clear();
            w->eventMouseDrag.m_event.clear();
        }
        mActionWidgets.clear();

        WidgetExtension* captionWidget = findFirstInTemplates("caption");
        mCaption = dynamic_cast<LuaText*>(captionWidget);

        if (mCaption)
            mActionWidgets.emplace(mCaption->widget(), mCaption);
        for (WidgetExtension* ext : findAllInTemplates("action"))
            mActionWidgets.emplace(ext->widget(), ext);

        for (auto& [w, _] : mActionWidgets)
        {
            w->eventMouseButtonPressed += MyGUI::newDelegate(this, &LuaWindow::notifyMousePress);
            w->eventMouseDrag += MyGUI::newDelegate(this, &LuaWindow::notifyMouseDrag);
        }

        WidgetExtension::updateTemplate();
    }

    void LuaWindow::updateProperties()
    {
        if (mCaption)
            mCaption->setCaption(propertyValue("caption", std::string()));
        mMoveResize = MyGUI::IntCoord();
        setForcedCoord(mMoveResize);

        WidgetExtension::updateProperties();
    }

    void LuaWindow::notifyMousePress(MyGUI::Widget* sender, int left, int top, MyGUI::MouseButton id)
    {
        if (id != MyGUI::MouseButton::Left)
            return;

        mPreviousMouse.left = left;
        mPreviousMouse.top = top;

        WidgetExtension* ext = mActionWidgets[sender];

        mChangeScale = MyGUI::IntCoord(
            ext->externalValue("move", MyGUI::IntPoint(1, 1)),
            ext->externalValue("resize", MyGUI::IntSize(0, 0)));
    }

    void LuaWindow::notifyMouseDrag(MyGUI::Widget* sender, int left, int top, MyGUI::MouseButton id)
    {
        if (id != MyGUI::MouseButton::Left)
            return;

        MyGUI::IntCoord change = mChangeScale;
        change.left *= (left - mPreviousMouse.left);
        change.top *= (top - mPreviousMouse.top);
        change.width *= (left - mPreviousMouse.left);
        change.height *= (top - mPreviousMouse.top);

        mMoveResize = mMoveResize + change.size();
        setForcedCoord(mMoveResize);
        // position can change based on size changes
        mMoveResize = mMoveResize + change.point() + getPosition() - calculateCoord().point();
        setForcedCoord(mMoveResize);
        updateCoord();

        mPreviousMouse.left = left;
        mPreviousMouse.top = top;

        sol::table table = makeTable();
        table["position"] = osg::Vec2f(mCoord.left, mCoord.top);
        table["size"] = osg::Vec2f(mCoord.width, mCoord.height);
        triggerEvent("windowDrag", table);
    }
}
