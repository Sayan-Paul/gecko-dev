/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * The contents of this file are subject to the Netscape Public
 * License Version 1.1 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy of
 * the License at http://www.mozilla.org/NPL/
 *
 * Software distributed under the License is distributed on an "AS
 * IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * rights and limitations under the License.
 *
 * The Original Code is mozilla.org code.
 *
 * The Initial Developer of the Original Code is Netscape
 * Communications Corporation.  Portions created by Netscape are
 * Copyright (C) 1998 Netscape Communications Corporation. All
 * Rights Reserved.
 *
 * Contributor(s): 
 *  Michael Lowe <michael.lowe@bigfoot.com>
 */

#include "nsWindowsTimer.h"
#include "nsITimerQueue.h"
#include "nsIWindowsTimerMap.h"
#include "nsCRT.h"
#include "prlog.h"
#include <stdio.h>
#include <windows.h>
#include <limits.h>
#include "nsWidgetsCID.h"
#include "nsIServiceManager.h"
#include <stdlib.h>


static NS_DEFINE_CID(kTimerManagerCID, NS_TIMERMANAGER_CID);


class nsTimer;


#define NS_PRIORITY_IMMEDIATE 10


void CALLBACK FireTimeout(HWND aWindow, 
                          UINT aMessage, 
                          UINT aTimerID, 
                          DWORD aTime)
{
  //  Don't allow old timer messages in here.
  if(aMessage != WM_TIMER) {
    PR_ASSERT(0);
    return;
  }

  nsresult rv;
  NS_WITH_SERVICE(nsIWindowsTimerMap, manager, kTimerManagerCID, &rv);
  if (NS_FAILED(rv)) return;

  nsTimer* timer = manager->GetTimer(aTimerID);
  if (timer == nsnull) {
    return;
  }

  if (timer->GetType() != NS_TYPE_REPEATING_PRECISE) {
    // stop OS timer: may be restarted later
    timer->KillOSTimer();
  }

  nsCOMPtr<nsITimerQueue> queue = do_QueryInterface(manager, &rv);
  if (NS_FAILED(rv)) return;

  MSG wmsg;
  if (!::PeekMessage(&wmsg, NULL, 0, 0, PM_NOREMOVE) ||
      timer->GetPriority() >= NS_PRIORITY_IMMEDIATE) {
    
    // fire timer immediatly
    timer->Fire();

    // while event queue is empty, fire off waiting timers
    while (queue->HasReadyTimers(NS_PRIORITY_LOWEST) &&
      !::PeekMessage(&wmsg, NULL, 0, 0, PM_NOREMOVE)) {

      queue->FireNextReadyTimer(NS_PRIORITY_LOWEST);
    }

  } else {
    // defer timer firing
    queue->AddReadyQueue(timer);
  }
}


NS_IMPL_ISUPPORTS1(nsTimer, nsITimer)


nsTimer::nsTimer() : nsITimer()
{
  NS_INIT_ISUPPORTS();

  mFunc = nsnull;
  mCallback = nsnull;
  mClosure = nsnull;
  mTimerID = 0;
  mTimerRunning = false;
}


nsTimer::~nsTimer()
{
  KillOSTimer();
  
  NS_IF_RELEASE(mCallback);
}


NS_IMETHODIMP_(void) nsTimer::SetPriority(PRUint32 aPriority)
{ 
  PR_ASSERT(aPriority >= NS_PRIORITY_LOWEST && 
            aPriority <= NS_PRIORITY_HIGHEST); 
  mPriority = aPriority; 
}


NS_IMETHODIMP nsTimer::Init(nsTimerCallbackFunc aFunc,
                void *aClosure,
                PRUint32 aDelay,
                PRUint32 aPriority,
                PRUint32 aType
                )
{
  mFunc = aFunc;
  mClosure = aClosure;

  return Init(aDelay, aPriority, aType);
}


NS_IMETHODIMP nsTimer::Init(nsITimerCallback *aCallback,
                PRUint32 aDelay,
                PRUint32 aPriority,
                PRUint32 aType
                )
{
  mCallback = aCallback;
  NS_ADDREF(mCallback);

  return Init(aDelay, aPriority, aType);
}


nsresult nsTimer::Init(PRUint32 aDelay,
                PRUint32 aPriority,
                PRUint32 aType
                )
{
  // prevent timer being released before
  // it has fired or is canceled
  NS_ADDREF_THIS();
  mTimerRunning = true;

  mDelay = aDelay;

  SetPriority(aPriority);
  SetType(aType);

  StartOSTimer(aDelay);

  return NS_OK;
}


void nsTimer::Fire()
{
  // prevent a canceled timer which is 
  // already in ready queue from firing
  if (mTimerRunning == false) return;

  // prevent notification routine 
  // from releasing timer by canceling it
  NS_ADDREF_THIS();

  // invoke notification routine
  if (mFunc != nsnull) {
    (*mFunc)(this, mClosure);
  }
  else if (mCallback != nsnull) {
    mCallback->Notify(this);
  }

  if (GetType() == NS_TYPE_REPEATING_SLACK) {
    // restart timer
    StartOSTimer(GetDelay());

  } else if (GetType() == NS_TYPE_ONE_SHOT) {

    // timer finished
    if (mTimerRunning == true) {
      mTimerRunning = false;

      NS_RELEASE_THIS();
    }
  }

  NS_RELEASE_THIS();
}


NS_IMETHODIMP_(void) nsTimer::Cancel()
{
  KillOSTimer();

  // timer finished
  if (mTimerRunning == true) {
    mTimerRunning = false;

    NS_RELEASE_THIS();
  }
}


void nsTimer::StartOSTimer(PRUint32 aDelay)
{
  PR_ASSERT(mTimerID == 0);

  nsresult rv;
  NS_WITH_SERVICE(nsIWindowsTimerMap, manager, kTimerManagerCID, &rv);
  if (NS_FAILED(rv)) return;

  // create OS timer
  mTimerID = ::SetTimer(NULL, 0, aDelay, (TIMERPROC)FireTimeout);

  // store mapping from OS timer to timer object
  manager->AddTimer(mTimerID, this);
}


void nsTimer::KillOSTimer()
{
  if (mTimerID != 0) {

    nsresult rv;
    NS_WITH_SERVICE(nsIWindowsTimerMap, manager, kTimerManagerCID, &rv);

    // remove mapping from OS timer to timer object
    if (NS_SUCCEEDED(rv)) {
      manager->RemoveTimer(mTimerID);
    }

    // kill OS timer
    ::KillTimer(NULL, mTimerID);

    mTimerID = 0;
  }
}
