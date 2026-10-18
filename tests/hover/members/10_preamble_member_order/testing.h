#pragma once

class Test {
public:
    virtual ~Test();
    virtual void SetUp();

private:
    int flag_saver_;
    struct Setup_should_be_spelled_SetUp {};
    virtual Setup_should_be_spelled_SetUp* Setup() { return nullptr; }
};
