#pragma once

#include <vector>

class Updatable
{
public:
    virtual ~Updatable() {};

    virtual void update(float deltaTime) = 0;
};

class UpdateManager
{
public:
    UpdateManager();
    ~UpdateManager();

    void update(float deltaTime);
    void add(Updatable *updatable);
    void remove(Updatable *updatable);

private:
    std::vector<Updatable *> m_updatables;
};
