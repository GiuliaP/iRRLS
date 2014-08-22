// Performs a Random Features mapping of the input given the projections vector
//
// Reads the projections from the configuration file RFmapper.ini and applies them to the incoming normalized samples
//


/* 
 * Copyright (C) 2014 iCub Facility - Istituto Italiano di Tecnologia
 * Author: Raffaello Camoriano
 * email: raffaello.camoriano@iit.it
 * website: www.robotcub.org
 * Permission is granted to copy, distribute, and/or modify this program
 * under the terms of the GNU General Public License, version 2 or any
 * later version published by the Free Software Foundation.
 *
 * A copy of the license can be found at
 * http://www.robotcub.org/icub/license/gpl.txt
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details
*/

/** 
\defgroup handCtrl
 
Example of grasping module based upon \ref ActionPrimitives 
library. 

Copyright (C) 2010 RobotCub Consortium
 
Author: Raffaello Camoriano

CopyPolicy: Released under the terms of the GNU GPL v2.0. 

\section intro_sec Description 
A module that makes use of \ref ActionPrimitives 
library to open and close either of the hands.

Scenario:

1) Both hands are opened at startup

2) A bottle containing the code of the hand to be closed
is received
 
3) The corresponding hand is closed by calling the close_hand sequence

4) The hand is opened again by the user via rpc:i
 
\section lib_sec Libraries 
- YARP libraries. 
- \ref ActionPrimitives library.  
- \ref SkinDyn library.  

#\section parameters_sec Parameters
 
\section portsa_sec Ports Accessed
The robot interface is assumed to be operative.
 
\section portsc_sec Ports Created 
Aside from the internal ports created by \ref ActionPrimitives 
library, we also have: 
 
- \e /handCtrl/handToBeClosed:i receives a bottle containing the code associated to the hand to close

 
- \e /handCtrl/rpc:i remote procedure call.

Recognized remote commands:
    - 'open_left_hand'
    - 'open_right_hand'
    - 'close_left_hand'
    - 'close_right_hand'

\section in_files_sec Input Data Files
None.

\section out_data_sec Output Data Files 
None. 
 
\section conf_file_sec Configuration Files 
--grasp_model_type \e type 
- specify the grasp model type according to the \ref 
  ActionPrimitives documentation.
 
--grasp_model_left_file \e file 
- specify the path to the file containing the grasp model 
  options for the left hand.

--grasp_model_right_file \e file 
- specify the path to the file containing the grasp model 
  options for the right hand.
 
--handCtrl_hand_sequences_file \e file 
- specify the path to the file containing the hand motion 
  sequences relative to the current context ( \ref
  ActionPrimitives ).
 
--from \e file 
- specify the configuration file (use \e --context option to 
  select the current context).
 
The configuration file passed through the option \e --from
should look like as follows:
 
\code 
[general]
// options used to open a ActionPrimitives object 
robot                           icub
thread_period                   50
default_exec_time               3.0
reach_tol                       0.007
verbosity                       on 
torso_pitch                     on
torso_roll                      off
torso_yaw                       on
torso_pitch_max                 30.0 
tracking_mode                   off 
verbosity                       on 
\endcode 

\section tested_os_sec Tested OS
Windows

\author Raffaello Camoriano
*/ 

#include <iostream>
#include <fstream>
#include <iomanip>
#include <string>
#include <deque>

#include <yarp/os/Network.h>
#include <yarp/os/RFModule.h>
#include <yarp/os/Bottle.h>
#include <yarp/os/BufferedPort.h>
#include <yarp/sig/Vector.h>
#include <yarp/os/Vocab.h>
#include <yarp/math/Math.h>
#include <yarp/dev/Drivers.h>
#include <yarp/conf/system.h>
#include <iCub/perception/models.h>

YARP_DECLARE_DEVICES(icubmod)

using namespace std;
using namespace yarp::os;
using namespace yarp::sig;
using namespace yarp::dev;
using namespace yarp::math;
using namespace iCub::perception;
using namespace iCub::action;
//using namespace iCub::skinDynLib;



/************************************************************************/
class RFmapper: public RFModule
{
protected:
    
    // Ports
    BufferedPort<Bottle>      outFeatures;
    BufferedPort<Bottle>      inFeatures;
    Port                      rpcPort;
    
    // Data
    int d;
    int t;
    int numRF;
    Bottle* proj;       // Pointer to the numRF-dimensional list of projections
    int mappingType;

public:
    /************************************************************************/
    RFmapper()
    {
    }

    // rpcPort commands handler
    bool respond(const Bottle &      command,
                 Bottle &      reply)
    {
        // This method is called when a command string is sent via RPC

        // Get command string
        string receivedCmd = command.get(0).asString().c_str();
       
        //int responseCode;   //Will contain Vocab-encoded response

        reply.clear();  // Clear reply bottle
        
        if (receivedCmd == "help")
        {
            reply.addVocab(Vocab::encode("many"));
            reply.addString("Available commands are:");
            reply.addString("help");
            reply.addString("quit");
        }
        else if (receivedCmd == "quit")
        {
            reply.addString("Quitting.");
            return false; //note also this
        }
        else
            reply.addString("Invalid command, type [help] for a list of accepted commands.");

        return true;
    }

    bool configure(ResourceFinder &rf)
    {
        string name=rf.find("name").asString().c_str();
        setName(name.c_str());

        Property config;
        config.fromConfigFile(rf.findFile("from").c_str());
        Bottle &bGeneral=config.findGroup("general");
        if (bGeneral.isNull())
        {
            printf("Error: group general is missing!\n");
            return false;
        }

        // Set dimensionalities
        d = rf.findGroup("general").check("d",Value(0)).asInt();
        t = rf.findGroup("general").check("t",Value(0)).asInt();
        numRF = rf.findGroup("general").check("numRF",Value(0)).asInt();
        
        if (d <= 0 || t <= 0 || numRF <= 0)
        {
            printf("Error: Inconsistent dimensionalities!\n");
            return false;
        }
        
        // Load precomputed projections
        proj = 0;
        Value* res;
        if (!rf.findGroup("proj").check("proj" , res))
        {
            printf("Error: Projections list missing!\n");
            return false;
        }
        proj = res->asList(); 
 
        if (proj->size() != numRF)
        {
            printf("Error: Inconsistent number of projections!\n");
            return false;
        }
        
        // Set mapping type
        mappingType = rf.findGroup("general").check("mappingType",Value(1)).asInt();
        
        // Open ports
        string fwslash="/";
        inFeatures.open((fwslash+name+"/features:i").c_str());
        printf("inFeatures opened\n");
        outFeatures.open((fwslash+name+"/features:o").c_str());
        printf("outFeatures opened\n");
        rpcPort.open((fwslash+name+"/rpc:i").c_str());
        printf("rpcPort opened\n");

        // Attach rpcPort to the respond() method
        attach(rpcPort);
        
        return true;
    }

    /************************************************************************/
    bool close()
    {        
        // Close ports
        inFeatures.close();
        printf("inFeatures port closed\n");
        outFeatures.close();
        printf("outFeatures port closed\n");
        rpcPort.close();
        printf("rpcPort port closed\n");

        return true;
    }

    /************************************************************************/
    double getPeriod()
    {
        // Period in seconds
        return 0.0;
    }

    /************************************************************************/
    void init()
    {
    }

    /************************************************************************/
    bool updateModule()
    {

        // Wait for closure command
        Bottle *b = inFeatures.read();    // blocking call

        if (b!=NULL)
        {
            // Apply random projections to incoming features
        }

        return true;
    }

    /************************************************************************/
    bool interruptModule()
    {
        // Interrupt any blocking reads on the input port
        inFeatures.interrupt();
        printf("inFeatures port interrupted\n");
        // Interrupt any blocking reads on the output port
        outFeatures.interrupt();
        printf("outFeatures port interrupted\n");

        // Interrupt any blocking reads on the rpc port        
        rpcPort.interrupt();
        printf("rpcPort interrupted\n");

        return true;
    }
};


/************************************************************************/
int main(int argc, char *argv[])
{
    Network yarp;
    if (!yarp.checkNetwork())
    {
        printf("YARP server not available!\n");
        return -1;
    }

    YARP_REGISTER_DEVICES(icubmod)

    ResourceFinder rf;
    rf.setVerbose(true);
    rf.setDefaultConfigFile("RFmapper_config.ini");
    rf.setDefaultContext("iRRLS");
    rf.setDefault("name","RFmapper");
    rf.configure(argc,argv);

    RFmapper mod;
    return mod.runModule(rf);
}
