/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <inttypes.h>
#include "JZApplication.h"
#include "main_window.h"

int main(int arg, char *argv[])
{
    JZApplication app;
    
    MainWindow w;    
    
    return app.exec();    
}