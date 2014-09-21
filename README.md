Simrep
======

windows kernel File redirection

功能说明
-----------------
1. 通过KernelUserManage模块，实现SID对用户名的转换，处理进程所属用户（在内核中是没有用户用户名的概念的，相应的是SID，通过内核用户管理模块可以实现SID与用用户名之间的转换。
注：内核程序并没有实时监控用户的新建、删除和修改!）,为重定向转存目录提供用户名。
2. 我们注册了IRP_MJ_CREATE、IRP_MJ_NEWWORK_QUERY_OPEN、IRP_MJ_READ、IRP_MJ_WRITE的预处理例程，但是只要是对IRP_MJ_CREATE的处理。在SimrepPreCreate中，我们对用户操作的目录进行判断，再根据需要将路径替换成重定向后的路径。

安装说明
-----------------
1. 将生成后的内核程序与Simrep.inf复制到目标系统中，右键simrep.inf，选择安装。
2. 通过net start simrep或者fltmc load simrep加载内核程序。
3. 通过fltmc attach simrep c:将simrep attach到C盘上
4. 最后可以通过fltmc detach simrep c:从C盘上detach,再通过net stop simrep或者fltmc unload simrep使其停止工作。
