#include <cstring>
#include <cerrno>
#include <sstream>
#include <iterator>
#include <memory>
#include <cstddef>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>

#include "dinit.h"
#include "service.h"
#include "dinit-log.h"
#include "dinit-socket.h"
#include "dinit-util.h"

/*
 * service.cc - Service management.
 * See service.h for details.
 */

// Find the requested service by name
static service_record * find_service(const std::list<service_record *> & records,
                                    const char *name) noexcept
{
    using std::list;
    list<service_record *>::const_iterator i = records.begin();
    for ( ; i != records.end(); i++ ) {
        if (strcmp((*i)->get_name().c_str(), name) == 0) {
            return *i;
        }
    }
    return nullptr;
}

service_record * service_set::find_service(const std::string &name) noexcept
{
    return ::find_service(records, name.c_str());
}

void service_set::stop_service(const std::string & name) noexcept
{
    service_record *record = find_service(name);
    if (record != nullptr) {
        record->stop();
        process_queues();
    }
}

// Called when a service has actually stopped; dependents have stopped already, unless this stop
// is due to an unexpected process termination.
void service_record::stopped() noexcept
{
    if (onstart_flags.runs_on_console) {
        tcsetpgrp(0, getpgrp());
        discard_console_log_buffer();
        release_console();
    }

    force_stop = false;

    // If we are a soft dependency of another target, break the acquisition from that target now:
    for (auto & dependent : dependents) {
        if (dependent->dep_type != dependency_type::REGULAR) {
            if (dependent->holding_acq) {
                dependent->holding_acq = false;
                release();
            }
        }
    }

    bool will_restart = (desired_state == service_state_t::STARTED)
            && services->get_auto_restart();

    for (auto dependency : depends_on) {
        // we signal dependencies in case they are waiting for us to stop:
        dependency.get_to()->dependent_stopped();
    }

    service_state = service_state_t::STOPPED;

    if (will_restart) {
        // Desired state is "started".
        restarting = true;
        start(false);
    }
    else {
        if (socket_fd != -1) {
            close(socket_fd);
            socket_fd = -1;
        }
        
        if (start_explicit) {
            start_explicit = false;
            release();
        }
        else if (required_by == 0) {
            services->service_inactive(this);
        }
    }

    log_service_stopped(service_name);
    notify_listeners(service_event_t::STOPPED);
}


bool service_record::do_auto_restart() noexcept
{
    if (auto_restart) {
        return services->get_auto_restart();
    }
    return false;
}

void service_record::emergency_stop() noexcept
{
    if (! do_auto_restart() && start_explicit) {
        start_explicit = false;
        release();
    }
    forced_stop();
    stop_dependents();
    stopped();
}


void service_record::require() noexcept
{
    if (required_by++ == 0) {
        prop_require = !prop_release;
        prop_release = false;
        services->add_prop_queue(this);
    }
}

void service_record::release() noexcept
{
    if (--required_by == 0) {
        desired_state = service_state_t::STOPPED;

        // Can stop, and can release dependencies now. We don't need to issue a release if
        // the require was pending though:
        prop_release = !prop_require;
        prop_require = false;
        services->add_prop_queue(this);

        if (service_state == service_state_t::STOPPED) {
            services->service_inactive(this);
        }
        else {
            do_stop();
        }
    }
}

void service_record::release_dependencies() noexcept
{
    for (auto & dependency : depends_on) {
        service_record * dep_to = dependency.get_to();
        if (dependency.holding_acq) {
            dep_to->release();
            dependency.holding_acq = false;
        }
    }
}

void service_record::start(bool activate) noexcept
{
    if (activate && ! start_explicit) {
        require();
        start_explicit = true;
    }
    
    if (desired_state == service_state_t::STARTED && service_state != service_state_t::STOPPED) return;

    bool was_active = service_state != service_state_t::STOPPED || desired_state != service_state_t::STOPPED;
    desired_state = service_state_t::STARTED;
    
    if (service_state != service_state_t::STOPPED) {
        // We're already starting/started, or we are stopping and need to wait for
        // that the complete.
        if (service_state != service_state_t::STOPPING || ! can_interrupt_stop()) {
            return;
        }
        // We're STOPPING, and that can be interrupted. Our dependencies might be STOPPING,
        // but if so they are waiting (for us), so they too can be instantly returned to
        // STARTING state.
        notify_listeners(service_event_t::STOPCANCELLED);
    }
    else if (! was_active) {
        services->service_active(this);
    }

    service_state = service_state_t::STARTING;
    waiting_for_deps = true;

    if (start_check_dependencies()) {
        services->add_transition_queue(this);
    }
}

void service_record::do_propagation() noexcept
{
    if (prop_require) {
        // Need to require all our dependencies
        for (auto & dep : depends_on) {
            dep.get_to()->require();
            dep.holding_acq = true;
        }
        prop_require = false;
    }
    
    if (prop_release) {
        release_dependencies();
        prop_release = false;
    }
    
    if (prop_failure) {
        prop_failure = false;
        failed_to_start(true);
    }
    
    if (prop_start) {
        prop_start = false;
        start(false);
    }

    if (prop_stop) {
        prop_stop = false;
        do_stop();
    }
}

void service_record::execute_transition() noexcept
{
    // state is STARTED with restarting set true if we are running a smooth recovery.
    if (service_state == service_state_t::STARTING || (service_state == service_state_t::STARTED
            && restarting)) {
        if (check_deps_started()) {
            bool have_console = service_state == service_state_t::STARTED && onstart_flags.runs_on_console;
            all_deps_started(have_console);
        }
    }
    else if (service_state == service_state_t::STOPPING) {
        if (stop_check_dependents()) {
            bring_down();
        }
    }
}

void service_record::do_start() noexcept
{
    if (pinned_stopped) return;
    
    if (service_state != service_state_t::STARTING) {
        return;
    }
    
    service_state = service_state_t::STARTING;

    waiting_for_deps = true;

    // Ask dependencies to start, mark them as being waited on.
    if (check_deps_started()) {
        // Once all dependencies are started, we start properly:
        all_deps_started();
    }
}

void service_record::dependency_started() noexcept
{
    if ((service_state == service_state_t::STARTING || service_state == service_state_t::STARTED)
            && waiting_for_deps) {
        services->add_transition_queue(this);
    }
}

bool service_record::start_check_dependencies() noexcept
{
    bool all_deps_started = true;

    for (auto & dep : depends_on) {
        service_record * to = dep.get_to();
        if (to->service_state != service_state_t::STARTED) {
            if (to->service_state != service_state_t::STARTING) {
                to->prop_start = true;
                services->add_prop_queue(to);
            }
            dep.waiting_on = true;
            all_deps_started = false;
        }
    }
    
    return all_deps_started;
}

bool service_record::check_deps_started() noexcept
{
    for (auto & dep : depends_on) {
        if (dep.waiting_on) {
            return false;
        }
    }

    return true;
}

bool service_record::open_socket() noexcept
{
    if (socket_path.empty() || socket_fd != -1) {
        // No socket, or already open
        return true;
    }
    
    const char * saddrname = socket_path.c_str();
    
    // Check the specified socket path
    struct stat stat_buf;
    if (stat(saddrname, &stat_buf) == 0) {
        if ((stat_buf.st_mode & S_IFSOCK) == 0) {
            // Not a socket
            log(loglevel_t::ERROR, service_name, ": Activation socket file exists (and is not a socket)");
            return false;
        }
    }
    else if (errno != ENOENT) {
        // Other error
        log(loglevel_t::ERROR, service_name, ": Error checking activation socket: ", strerror(errno));
        return false;
    }

    // Remove stale socket file (if it exists).
    // We won't test the return from unlink - if it fails other than due to ENOENT, we should get an
    // error when we try to create the socket anyway.
    unlink(saddrname);

    uint sockaddr_size = offsetof(struct sockaddr_un, sun_path) + socket_path.length() + 1;
    struct sockaddr_un * name = static_cast<sockaddr_un *>(malloc(sockaddr_size));
    if (name == nullptr) {
        log(loglevel_t::ERROR, service_name, ": Opening activation socket: out of memory");
        return false;
    }

    name->sun_family = AF_UNIX;
    strcpy(name->sun_path, saddrname);

    int sockfd = dinit_socket(AF_UNIX, SOCK_STREAM, 0, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (sockfd == -1) {
        log(loglevel_t::ERROR, service_name, ": Error creating activation socket: ", strerror(errno));
        free(name);
        return false;
    }

    if (bind(sockfd, (struct sockaddr *) name, sockaddr_size) == -1) {
        log(loglevel_t::ERROR, service_name, ": Error binding activation socket: ", strerror(errno));
        close(sockfd);
        free(name);
        return false;
    }
    
    free(name);
    
    // POSIX (1003.1, 2013) says that fchown and fchmod don't necessarily work on sockets. We have to
    // use chown and chmod instead.
    if (chown(saddrname, socket_uid, socket_gid)) {
        log(loglevel_t::ERROR, service_name, ": Error setting activation socket owner/group: ", strerror(errno));
        close(sockfd);
        return false;
    }
    
    if (chmod(saddrname, socket_perms) == -1) {
        log(loglevel_t::ERROR, service_name, ": Error setting activation socket permissions: ", strerror(errno));
        close(sockfd);
        return false;
    }

    if (listen(sockfd, 128) == -1) { // 128 "seems reasonable".
        log(loglevel_t::ERROR, ": Error listening on activation socket: ", strerror(errno));
        close(sockfd);
        return false;
    }
    
    socket_fd = sockfd;
    return true;
}

void service_record::all_deps_started(bool has_console) noexcept
{
    if (onstart_flags.starts_on_console && ! has_console) {
        waiting_for_deps = true;
        queue_for_console();
        return;
    }
    
    waiting_for_deps = false;

    if (! can_proceed_to_start()) {
        waiting_for_deps = true;
        return;
    }

    if (! open_socket()) {
        failed_to_start();
    }

    bool start_success = bring_up();
    if (! start_success) {
        failed_to_start();
    }
}

void service_record::acquired_console() noexcept
{
    if (service_state != service_state_t::STARTING) {
        // We got the console but no longer want it.
        release_console();
    }
    else if (check_deps_started()) {
        all_deps_started(true);
    }
    else {
        // We got the console but can't use it yet.
        release_console();
    }
}


void service_record::started() noexcept
{
    if (onstart_flags.starts_on_console && ! onstart_flags.runs_on_console) {
        tcsetpgrp(0, getpgrp());
        release_console();
    }

    log_service_started(get_name());
    service_state = service_state_t::STARTED;
    notify_listeners(service_event_t::STARTED);

    if (onstart_flags.rw_ready) {
        open_control_socket();
    }
    if (onstart_flags.log_ready) {
        setup_external_log();
    }

    if (force_stop || desired_state == service_state_t::STOPPED) {
        // We must now stop.
        do_stop();
        return;
    }

    // Notify any dependents whose desired state is STARTED:
    for (auto dept : dependents) {
        dept->get_from()->dependency_started();
        dept->waiting_on = false;
    }
}

void service_record::failed_to_start(bool depfailed) noexcept
{
    if (!depfailed && onstart_flags.starts_on_console) {
        tcsetpgrp(0, getpgrp());
        release_console();
    }
    
    log_service_failed(get_name());
    service_state = service_state_t::STOPPED;
    if (start_explicit) {
        start_explicit = false;
        release();
    }
    notify_listeners(service_event_t::FAILEDSTART);
    
    // Cancel start of dependents:
    for (auto & dept : dependents) {
        switch (dept->dep_type) {
        case dependency_type::REGULAR:
        case dependency_type::MILESTONE:
            if (dept->get_from()->service_state == service_state_t::STARTING) {
                dept->get_from()->prop_failure = true;
                services->add_prop_queue(dept->get_from());
            }
            break;
        case dependency_type::WAITS_FOR:
        case dependency_type::SOFT:
            if (dept->waiting_on) {
                dept->waiting_on = false;
                dept->get_from()->dependency_started();
            }
            if (dept->holding_acq) {
                dept->holding_acq = false;
                release();
            }
        }
    }
}

bool service_record::bring_up() noexcept
{
    // default implementation: there is no process, so we are started.
    started();
    return true;
}

// Mark this and all dependent services as force-stopped.
void service_record::forced_stop() noexcept
{
    if (service_state != service_state_t::STOPPED) {
        force_stop = true;
        services->add_transition_queue(this);
    }
}

void service_record::dependent_stopped() noexcept
{
    if (service_state == service_state_t::STOPPING && waiting_for_deps) {
        services->add_transition_queue(this);
    }
}

void service_record::stop(bool bring_down) noexcept
{
    if (start_explicit) {
        start_explicit = false;
        release();
    }

    if (bring_down) {
        do_stop();
    }
}

void service_record::do_stop() noexcept
{
    if (pinned_started) return;

    if (start_explicit && ! do_auto_restart()) {
        start_explicit = false;
        release();
        if (required_by == 0) return; // release will re-call us anyway
    }

    bool all_deps_stopped = stop_dependents();

    if (service_state != service_state_t::STARTED) {
        if (service_state == service_state_t::STARTING) {
            if (! waiting_for_deps) {
                if (! can_interrupt_start()) {
                    // Well this is awkward: we're going to have to continue starting. We can stop once we've
                    // reached the started state.
                    return;
                }

                if (! interrupt_start()) {
                    // Now wait for service startup to actually end; we don't need to handle it here.
                    return;
                }
            }

            // We must have had desired_state == STARTED.
            notify_listeners(service_event_t::STARTCANCELLED);

            // Reaching this point, we are starting interruptibly - so we
            // stop now (by falling through to below).
        }
        else {
            // If we're starting we need to wait for that to complete.
            // If we're already stopping/stopped there's nothing to do.
            return;
        }
    }

    service_state = service_state_t::STOPPING;
    waiting_for_deps = true;
    if (all_deps_stopped) {
        services->add_transition_queue(this);
    }
}

bool service_record::stop_check_dependents() noexcept
{
    bool all_deps_stopped = true;
    for (auto dept : dependents) {
        if (dept->dep_type == dependency_type::REGULAR && ! dept->get_from()->is_stopped()) {
            all_deps_stopped = false;
            break;
        }
    }
    
    return all_deps_stopped;
}

bool service_record::stop_dependents() noexcept
{
    bool all_deps_stopped = true;
    for (auto dept : dependents) {
        if (dept->dep_type == dependency_type::REGULAR) {
            if (! dept->get_from()->is_stopped()) {
                // Note we check *first* since if the dependent service is not stopped,
                // 1. We will issue a stop to it shortly and
                // 2. It will notify us when stopped, at which point the stop_check_dependents()
                //    check is run anyway.
                all_deps_stopped = false;
            }

            if (force_stop) {
                // If this service is to be forcefully stopped, dependents must also be.
                dept->get_from()->forced_stop();
            }

            dept->get_from()->prop_stop = true;
            services->add_prop_queue(dept->get_from());
        }
    }

    return all_deps_stopped;
}

// All dependents have stopped; we can stop now, too. Only called when STOPPING.
void service_record::bring_down() noexcept
{
    waiting_for_deps = false;
    stopped();
}

void service_record::unpin() noexcept
{
    if (pinned_started) {
        pinned_started = false;
        if (desired_state == service_state_t::STOPPED || force_stop) {
            do_stop();
            services->process_queues();
        }
    }
    if (pinned_stopped) {
        pinned_stopped = false;
        if (desired_state == service_state_t::STARTED) {
            do_start();
            services->process_queues();
        }
    }
}

void service_record::queue_for_console() noexcept
{
    services->append_console_queue(this);
}

void service_record::release_console() noexcept
{
    services->pull_console_queue();
}

bool service_record::interrupt_start() noexcept
{
    services->unqueue_console(this);
    return true;
}

void service_set::service_active(service_record *sr) noexcept
{
    active_services++;
}

void service_set::service_inactive(service_record *sr) noexcept
{
    active_services--;
}
