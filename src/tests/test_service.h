#include "service.h"

class test_service : public service_record
{
    public:
    test_service(service_set *set, std::string name, service_type_t type_p,
            const std::list<prelim_dep> &deplist_p)
            : service_record(set, name, type_p, deplist_p)
    {

    }

    // Do any post-dependency startup; return false on failure
    virtual bool bring_up() noexcept override
    {
        // return service_record::bring_up();
        return true;
    }

    // All dependents have stopped.
    virtual void bring_down() noexcept override
    {
        return service_record::bring_down();
    }

    // Whether a STARTING service can immediately transition to STOPPED (as opposed to
    // having to wait for it reach STARTED and then go through STOPPING).
    virtual bool can_interrupt_start() noexcept override
    {
        return waiting_for_deps;
    }

    virtual bool interrupt_start() noexcept override
    {
        return true;
    }

    void started() noexcept
    {
        service_record::started();
    }

    void failed_to_start() noexcept
    {
        service_record::failed_to_start();
    }
};
