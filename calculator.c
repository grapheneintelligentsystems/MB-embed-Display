/*
 * calculator.c:
 *      Communicate 4D systems display with Raspberry Pi and control the 
 *      GPIO through the display
 * 
 *	Michail Beliatis, August 2017
 ***********************************************************************
 */

#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <pthread.h>
#include <math.h>

#include <geniePi.h>

#ifndef	TRUE
#  define TRUE  (1==1)
#  define FALSE (1==2)
#endif

// Calculator globals

double acc ;
double memory ;
double display = 0.0 ;
int    lastOperator ;
int    errorCondition  ;


/*
 * clockForm:
 *	This is run as a concurrent thread and will update the
 *	clock form on the display continually.
 *********************************************************************************
 */

static void *clockForm (void *data)
{
  struct sched_param sched ;
  int pri = 10 ;

  time_t tt ;
  struct tm timeData ;

// Set to a real-time priority
//	(only works if root, ignored otherwise)

  memset (&sched, 0, sizeof(sched)) ;

  if (pri > sched_get_priority_max (SCHED_RR))
    pri = sched_get_priority_max (SCHED_RR) ;

  sched.sched_priority = pri ;
  sched_setscheduler (0, SCHED_RR, &sched) ;

// What's the time, Mr Wolf?

  sleep (1) ;

  for (;;)
  {
    tt = time (NULL) ;
    while (time (NULL) == tt)
      usleep (100000) ;	// 100mS - So it might be 1/10 sec. slow...

    tt = time (NULL) ;
    (void)localtime_r (&tt, &timeData) ;
    genieWriteObj (GENIE_OBJ_LED_DIGITS, 0, timeData.tm_hour * 100 + timeData.tm_min) ;
    genieWriteObj (GENIE_OBJ_LED_DIGITS, 1, timeData.tm_sec) ;
  }

 return (void *)NULL ;
}



/*
 * updateDisplay:
 *	Do just that.
 *********************************************************************************
 */

void updateDisplay (void)
{
  char buf [32] ;

  if (errorCondition)
    sprintf (buf, "%s", "ERROR") ;
  else
  {
    sprintf (buf, "%11.9g", display) ;
    printf ("%s\n", buf) ;
  }

  genieWriteStr (0, buf) ;	// Text box number 0

  sprintf (buf, "%c %c",
	memory != 0.0 ? 'M' : ' ',
	lastOperator == 0 ? ' ' : lastOperator) ;

  genieWriteStr (1, buf) ;	// Text box number 1
}


/*
 * processOperator:
 *	Take our operator and apply it to the accumulator and display registers
 *********************************************************************************
 */

static void processOperator (int operator)
{
  /**/ if (operator == '+')
    acc += display ;
  else if (operator == '-')
    acc -= display ;
  else if (operator == '*')
    acc *= display ;
  else if (operator == '/')
  {
    if (display == 0.0)
      errorCondition = TRUE ;
    acc /= display ;
  }
  display = acc ;

  if (fabs (display) > 999999999.0)
    errorCondition = TRUE ;

  if (fabs (display) < 0.00000001)
    display = 0.0 ;
}


/*
 * calculatorKey:
 *	We've pressed a key on our calculator.
 *********************************************************************************
 */

void calculatorKey (int key)
{
  static int gotDecimal     = FALSE ;
  static int startNewNumber = TRUE ;
  static double multiplier  = 1.0 ;
  float digit ;

// Eeeeee...

  if (errorCondition && key != 'a')
    return ;

  if (isdigit (key))
  {
    if (startNewNumber)
    {
      startNewNumber = FALSE ;
      multiplier     = 1.0 ;
      display        = 0.0 ;
    }

    digit = (double)(key - '0') ;
    if (multiplier == 1.0)
      display = display * 10 + (double)digit ;
    else
    {
      display     = display + (multiplier * digit) ;
      multiplier /= 10.0 ;
    }
    updateDisplay () ;
    return ;
  }

  switch (key)
  {
    case 'a':			// AC - All Clear
      lastOperator   = 0 ;
      acc            = 0.0 ;
      memory         = 0.0 ;
      display        = 0.0 ;
      gotDecimal     = FALSE ;
      errorCondition = FALSE ;
      startNewNumber = TRUE ;
      break ;

    case 'c':			// Clear entry or operator
      if (lastOperator != 0)
	lastOperator = 0 ;
      else
      {
	display        = 0.0 ;
	gotDecimal     = FALSE ;
	startNewNumber = TRUE ;
      }
      break ;

// Memory keys

    case 128:		// Mem Store
      memory = display ;
      break ;

    case 129:		// M+
      memory += display ;
      break ;

    case 130:		// M-
      memory -= display ;
      break ;

    case 131:		// MR - Memory Recall
      display = memory ;
      break ;

    case 132:		// MC - Memory Clear
      memory = 0.0 ;
      break ;

// Other functions

    case 140:		// +/-
      display = -display ;
      break ;

    case 's':	// Square root
      if (display < 0.0)
	errorCondition = TRUE ;
      else
      {
	display        = sqrt (display) ;
	gotDecimal     = FALSE ;
	startNewNumber = TRUE ;
      }
      break ;

// Operators

    case '+': case '-': case '*': case '/':
      if (lastOperator == 0)
	acc = display ;
      else
	processOperator (lastOperator) ;
      lastOperator    = key ;
      startNewNumber = TRUE ;
      gotDecimal     = FALSE ;
      break ;

    case '=':
      if (lastOperator != 0)
	processOperator (lastOperator) ;
      lastOperator    = 0 ;
      gotDecimal     = FALSE ;
      startNewNumber = TRUE ;
      acc            = 0.0 ;
      break ;

    case '.':
      if (!gotDecimal)
      {
	if (startNewNumber)
	{
	  startNewNumber = FALSE ;
	  display        = 0.0 ;
	}
	multiplier = 0.1 ;
	gotDecimal = TRUE ;
	break ;
      }

    default:
      printf ("*** Unknown key from display: 0x%02X\n", key) ;
      break ;
  }

  updateDisplay () ;
}


/*
 * handleGenieEvent:
 *	Take a reply off the display and call the appropriate handler for it.
 *********************************************************************************
 */

void handleGenieEvent (struct genieReplyStruct *reply)
{
  if (reply->cmd != GENIE_REPORT_EVENT)
  {
    printf ("Invalid event from the display: 0x%02X\r\n", reply->cmd) ;
    return ;
  }

  /**/ if (reply->object == GENIE_OBJ_KEYBOARD)
  {
    if (reply->index == 0)	// Only one keyboard
      calculatorKey (reply->data) ;
    else
      printf ("Unknown keyboard: %d\n", reply->index) ;
  }
  else if (reply->object == GENIE_OBJ_WINBUTTON)
  {
    /**/ if (reply->index == 1)	// Clock button on main display
      genieWriteObj (GENIE_OBJ_FORM, 1, 0) ;
    else if (reply->index == 0)	// Calculator button on clock display
    {
      genieWriteObj (GENIE_OBJ_FORM, 0, 0) ;
      updateDisplay () ;
    }
    else
      printf ("Unknown button: %d\n", reply->index) ;
  }
  else
    printf ("Unhandled Event: object: %2d, index: %d data: %d [%02X %02X %04X]\r\n",
      reply->object, reply->index, reply->data, reply->object, reply->index, reply->data) ;
}


/*
 *********************************************************************************
 * main:
 *	Run our little demo
 *********************************************************************************
 */

int main ()
{
  pthread_t myThread ;
  struct genieReplyStruct reply ;

  printf ("\n\n\n\n") ;
  printf ("Visi-Genie Calculator Demo\n") ;
  printf ("==========================\n") ;

// Genie display setup
//	Using the Raspberry Pi's on-board serial port.

  if (genieSetup ("/dev/ttyAMA0", 115200) < 0)
  {
    fprintf (stderr, "rgb: Can't initialise Genie Display: %s\n", strerror (errno)) ;
    return 1 ;
  }

// Select form 0 (the calculator)

  genieWriteObj (GENIE_OBJ_FORM, 0, 0) ;

  calculatorKey ('a') ;	// Clear the calculator

// Start the clock thread

  (void)pthread_create (&myThread, NULL, clockForm, NULL) ;

// Big loop - just wait for events from the display now

  for (;;)
  {
    while (genieReplyAvail ())
    {
      genieGetReply    (&reply) ;
      handleGenieEvent (&reply) ;
    }
    usleep (10000) ; // 10mS - Don't hog the CPU in-case anything else is happening...
  }

  return 0 ;
}
