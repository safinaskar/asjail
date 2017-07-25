/* Backend tool */
/* Программа нужна как минимум потому, что один раз не сработал systemd-nspawn, а asjail-max сработал (один раз так было из-за того, что там было что-то смонтировано, reported here: https://github.com/systemd/systemd/issues/3695 ) */
/* Существует программа unshare, так что эта программа deprecated */

/* поддерживает только архитектуры, где стек растёт вниз (т. е. почти все) */
/* Мы не подменяем текущий процесс (как это делает chroot), то есть PID другой */
/* Новые ядра поддерживают всё больше и больше опций */
/* CLONE_NEWNET позволяет слушать занятый порт и делает интернет недоступным внутри asjail */

/*
 * Список опций, упомянутых в man page от 2010-09-10 (те из них, которые пригодны для создания контейнеров, отмечены; те, что непригодны - не отмечены; точно):
 * CLONE_CHILD_CLEARTID
 * CLONE_CHILD_SETTID
 * CLONE_FILES
 * CLONE_FS
 * CLONE_IO
 * CLONE_NEWIPC [*]
 * CLONE_NEWNET [*]
 * CLONE_NEWNS [*]
 * CLONE_NEWPID [*]
 * CLONE_NEWUTS [*]
 * CLONE_PARENT
 * CLONE_PARENT_SETTID
 * CLONE_PID
 * CLONE_PTRACE
 * CLONE_SETTLS
 * CLONE_SIGHAND
 * CLONE_STOPPED
 * CLONE_SYSVSEM
 * CLONE_THREAD
 * CLONE_UNTRACES
 * CLONE_VFORK
 * CLONE_VM
 * Все отмеченные опции требуют прав рута (а точнее, определённых CAP_*)
 */

/* TO DO: сделать понятные сообщения об ошибках */
/* TO DO: ядро может быть без поддержки некоторых опций (при конфигурировании не заданы) */
/* TO DO: с сигналами более-менее разобрался, но всё же: когда я нажимаю Ctrl-C, сигнал отсылается всей группе? А если не с клавиатуры, то не всей? А можно вообще послать не всей? */
/* TO DO: на самом деле надо было сделать cgroup, чтобы процесс не смог от нас отфоркнуться (хотя можно просто всегда использовать CLONE_NEWPID) */
/* TO DO: почему-то CLONE_NEWUSER не работает на ядре 3.9-rc5 с допкоммитами на Sid-2013-Apr в Qemu */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

#include <unistd.h>
#include <sys/wait.h>

#include <sched.h>

#include <libsh.h>
#include <libsh/cxx.hpp>

/* Yes, this is (probably) bad function */
#define dbg(...) (fprintf(stderr, "asjail: debug: " __VA_ARGS__), fputc('\n', stderr), (void)0)

using namespace std::literals;

sigset_t all_but_chld_and_term;

static int fn(void *argv)
{
  sh_reset();
  sh_set_terminate(&sh_exit_failure);

  dbg("child: started; setting procmask");
  sh_x_sigprocmask(SIG_UNBLOCK, &all_but_chld_and_term, NULL);

  dbg("child: execvp");
  char *const *cargv = (char **)argv;
  sh_x_execvp(cargv[0], cargv);

  return 1; /* To make compiler happy */
}

pid_t child = -1;

static void kill_child(int signum /* unused */)
{
  if (child == -1)
    {
      dbg("основной процесс убит до того, как успел создать дочерний процесс");
      exit (EXIT_FAILURE); /* Нас убили до того, как мы успели запустить процесс */
    }

  dbg("got signal, убиваем дочерний процесс");
  kill(child, SIGKILL); /* Не проверяем код возврата, так как процесс может уже завершиться (а мы ещё не успели убрать обработчик) */
}

int main(int, char *argv[])
{
  int flags = SIGCHLD;

  sh_init (argv[0]);
  sh_arg_parse (&argv, "Usage: "s + sh_get_program () + " [OPTION]... PROGRAM [ARG]...\n"s, "",
    sh_arg_make_opt ({'i'}, {"ipc"  }, sh_arg_optional, [&flags](const char *){ flags |= CLONE_NEWIPC;  }, NULL, "CLONE_NEWIPC"),
    sh_arg_make_opt ({'n'}, {"net"  }, sh_arg_optional, [&flags](const char *){ flags |= CLONE_NEWNET;  }, NULL, "CLONE_NEWNET"),
    sh_arg_make_opt ({'m'}, {"mount"}, sh_arg_optional, [&flags](const char *){ flags |= CLONE_NEWNS;   }, NULL, "CLONE_NEWNS (mount name space)"),
    sh_arg_make_opt ({'p'}, {"pid"  }, sh_arg_optional, [&flags](const char *){ flags |= CLONE_NEWPID;  }, NULL, "CLONE_NEWPID"),
    sh_arg_make_opt ({'u'}, {"uts"  }, sh_arg_optional, [&flags](const char *){ flags |= CLONE_NEWUTS;  }, NULL, "CLONE_NEWUTS"),
    sh_arg_make_opt ({'U'}, {"user" }, sh_arg_optional, [&flags](const char *){ flags |= CLONE_NEWUSER; }, NULL, "CLONE_NEWUSER")
  );

  if (argv[0] == NULL)
    {
      sh_throwx ("no program");
    }

  if (sigfillset(&all_but_chld_and_term) == -1)
    {
      sh_throw("cannot initialize signal set; this should never happen");
    }

  sigdelset(&all_but_chld_and_term, SIGCHLD);
  sigdelset(&all_but_chld_and_term, SIGTERM);

  dbg("sigprocmask");
  sh_x_sigprocmask(SIG_BLOCK, &all_but_chld_and_term, NULL);

  dbg("signal");
  signal(SIGTERM, &kill_child); /* TO DO: use sigaction */

  const int stack_size = 65536; /* TO DO: why? */
  void *child_stack = sh_x_malloc(stack_size);

  /* Внутри child не нужно перемонтировать /proc (если используется CLONE_NEWNET). Он и так содержит новую информацию в /proc/net. А вот при использовании CLONE_NEWPID /proc надо перемонтировать */
  /* Более того, если в child с NEWPID смонтировать /proc, то он будет выглядеть снаружи довольно странно */
  dbg("clone");
  child = clone(&fn, (char *)child_stack + stack_size, flags, argv);

  if (child == -1)
    {
      sh_throw("clone");
    }

  int status;
  dbg("waitpid");
  sh_x_waitpid(child, &status, 0);

  dbg("waited, restore sigprocmask and signal");
  sh_x_sigprocmask(SIG_UNBLOCK, &all_but_chld_and_term, NULL);
  signal(SIGTERM, SIG_DFL);

  if (WIFEXITED(status))
    {
      exit (WEXITSTATUS(status));
    }
  else if (WIFSIGNALED(status))
    {
      signal(WTERMSIG(status), SIG_DFL); /* We don't check return value, because it may be SIGKILL */

      /* TO DO: если он сгеренил core dump, то мне это делать уже не нужно */
      if (kill(getpid(), WTERMSIG(status)) == -1)
        {
          sh_throw("I cannot kill myself; this should never happen");
        }

      pause();
    }
  else
    {
      sh_throwx("bug: child didn't exit and isn't signaled"); /* TO DO: is this possible? is WIFCONTINUED possible? */
    }

  /* NOTREACHED */
}
