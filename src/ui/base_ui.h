#pragma once

class BaseUI
{
public:
    virtual ~BaseUI() {};
    virtual void renderUI() {};
    virtual void renderOverlay() {};
};
