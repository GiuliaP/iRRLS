
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

#include <iostream>
#include <fstream>
#include <iomanip>
#include <string>

#include "gurls++/recrlswrapperchol.h"
#include "gurls++/rlsprimal.h"
#include "gurls++/primal.h"

#include <yarp/os/Network.h>
#include <yarp/os/RFModule.h>
#include <yarp/os/Bottle.h>
#include <yarp/os/BufferedPort.h>
#include <yarp/sig/Vector.h>
#include <yarp/os/Vocab.h>
#include <yarp/math/Math.h>
#include <yarp/conf/system.h>

using namespace std;
using namespace yarp::os;
using namespace yarp::sig;
using namespace yarp::math;
using namespace gurls;

typedef double T;

/************************************************************************/
class RRLSestimator: public RFModule
{
protected:
    
    // Ports
    BufferedPort<Bottle>      inVec;
    //BufferedPort<Vector>      inVec;
    BufferedPort<Bottle>      pred;
    BufferedPort<Bottle>      perf;
    Port                      rpcPort;
    
    // Data
    bool verbose;
    int d;
    int t;
    string perfType;
    int pretrain;               // Preliminary batch training required
    string pretrainFile;        // Preliminary batch training file
    int n_pretr;                // Number of pretraining samples
    long unsigned int updateCount ;
       
    gMat2D<T> trainSet;    
    gMat2D<T> Xtr;    
    gMat2D<T> ytr;    
    RecursiveRLSCholUpdateWrapper<T> estimator;
    gMat2D<T> varCols;          // Matrix containing the column-wise variances computed on the training set


public:
    /************************************************************************/
    RRLSestimator() : estimator("recursiveRLSChol")
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
        
        // Set verbosity
        verbose = rf.check("verbose",Value(0)).asInt();
               
        // Set dimensionalities
        d = rf.check("d",Value(0)).asInt();
        t = rf.check("t",Value(0)).asInt();
        
        if (d <= 0 || t <= 0 )
        {
            printf("Error: Inconsistent dimensionalities!\n");
            return false;
        }
        
        // Set perf type
        perfType = rf.check("perf",Value("RMSE")).asString();
        
        if ( perfType != "RMSE" )
        {
            printf("Error: Inconsistent performance measure! Set to RMSE.\n");
            perfType = "RMSE";
        }
        
        // Set preliminary batch training preferences
        pretrain = rf.check("pretrain",Value("0")).asInt();
        
        if ( pretrain == 1 )
        {            
            // Set preliminary batch training file path
            pretrainFile = rf.check("pretrain",Value("icubdyn.dat")).asString();
            n_pretr = rf.check("n_pretr",Value("2")).asInt();
        }
        
        // Print Configuration
        cout << endl << "-------------------------" << endl;
        cout << "Configuration parameters:" << endl << endl;
        cout << "d = " << d << endl;
        cout << "t = " << t << endl;
        cout << "perf = " << perfType << endl;
        if ( pretrain == 1 )
        {
            printf("Pretraining requested\n");
            printf("Pretraining file name set to: %s\n", pretrainFile.c_str());
            printf("Number of pretraining samples: %d\n", n_pretr);
        }
        cout << "-------------------------" << endl << endl;
       
        // Open ports
    
        string fwslash="/";
        inVec.open((fwslash+name+"/vec:i").c_str());
        printf("inVec opened\n");
        
        pred.open((fwslash+name+"/pred:o").c_str());
        printf("pred opened\n");
        
        perf.open((fwslash+name+"/perf:o").c_str());
        printf("perf opened\n");
        
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
        inVec.close();
        printf("inVec closed\n");
        
        pred.close();
        printf("pred closed\n");
        
        perf.close();
        printf("perf closed\n");
        
        rpcPort.close();
        printf("rpcPort closed\n");

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
        srand(static_cast<unsigned int>(time(NULL)));

        updateCount = 0;
        
        //Pre-training from file?
        if ( pretrain == 1 )
        {
            string trainFilePath = "data/" + pretrainFile;
            
            
            
            try
            {
                // Load data files
                cout << "Loading data files..." << endl;
                trainSet.readCSV(trainFilePath);
                Xtr.submatrix(trainSet , n_pretr , d);
                for ( int i = 0 ; i < t ; ++i )
                {
                    gVec<T> tmpCol = trainSet(d + i);
                    ytr.setColumn( tmpCol , (long unsigned int) i);
                }

                // Compute variance for each output on the training set
                gMat2D<T> varCols = gMat2D<T>::zeros(1,t);
                //varCols(zerosMat);
                gVec<T>* sumCols_v = ytr.sum(COLUMNWISE);          // Vector containing the column-wise sum
                gMat2D<T> meanCols(sumCols_v->getData(), 1, t, 1); // Matrix containing the column-wise sum
                meanCols /= n_pretr;        // Matrix containing the column-wise mean
                
                if (verbose) cout << "Mean of the output columns: " << endl << meanCols << endl;
                
                for (int i = 0; i < n_pretr; i++)
                {
                    gMat2D<T> ytri(ytr[i].getData(), 1, t, 1);
                    varCols += (ytri - meanCols) * (ytri - meanCols); // NOTE: Temporary assignment
                }
                varCols /= n_pretr;     // Compute variance
                if (verbose) cout << "Variance of the output columns: " << endl << varCols << endl;

                // Initialize model
                cout << "Batch pretraining the RLS model with " << n_pretr << " samples." << endl;
                estimator.train(Xtr, ytr);
            }
            
            catch (gException& e)
            {
                cout << e.getMessage() << endl;
                //return false;   // NOTE: May be worth to set up specific error return values
            }
        }
    }

    /************************************************************************/
    bool updateModule()
    {
        updateCount++;
        // Recursive update support and storage variables declaration and initialization
        gMat2D<T> Xnew(1,d);
        gMat2D<T> ynew(1,t);
        gVec<T> Xnew_v(d);
        gVec<T> ynew_v(t);
        //gMat2D<T> yte_pred(nte,t);
        gMat2D<T> *resptr = 0;
        gMat2D<T> nSE(gMat2D<T>::zeros(1, t));
//         gMat2D<T> nMSE_rec(gMat2D<T>::zeros(nte, t));
        
        // Wait for input feature vector
        if(verbose) cout << "Expecting input vector" << endl;
        
        Bottle *bin = inVec.read();    // blocking call
        //Vector *bin = inVec.read();    // blocking call
        
        if(verbose) cout << "Got it!" << endl << bin->toString() << endl;

        if (bin != 0)
        {
            Bottle& bpred = pred.prepare(); // Get a place to store things.
            bpred.clear();  // clear is important - b might be a reused object
            
            Bottle& bperf = perf.prepare(); // Get a place to store things.
            bperf.clear();  // clear is important - b might be a reused object
    
            //Store the received sample in gMat2D format for it to be compatible with gurls++
            for (int i = 0 ; i < bin->size() ; ++i)
            {
                if ( i < d )
                {
                    Xnew[i] = bin->get(i).asDouble();
                    //Xnew[i] = (*bin)[i];
                }
                else if ( (i>=d) && (i<d+t) )
                {
                    ynew[i - d] = bin->get(i).asDouble();
                    //ynew[i - d] = (*bin)[i];
                }
            }
            
            //-----------------------------------
            //          Prediction
            //-----------------------------------
            
            // Test on the incoming sample
            resptr = estimator.eval(Xnew);
            
            // Store result in matrix yte_pred WARNING: to be removed
            //copy(yte_pred.getData() + i , resptr->getData(), t , nte, 1 );
            for (int i = 0 ; i < t ; ++i)
            {
                bpred.addDouble((*resptr)(1 , i));
            }
            if(verbose) printf("Sending prediction: %s\n", bpred.toString().c_str());
            pred.write();
            
            // Compute nMSE and store
            //WARNING: "/" operator works like matlab's "\".
            nSE += varCols / ( ynew - *resptr )*( ynew - *resptr ) ;   

            gMat2D<T> tmp = nSE  / (updateCount+1);
            //copy(nMSE_rec.getData() + i, tmp.getData(), t, nte, 1);
            for (int i = 0 ; i < t ; ++i)
            {
                bperf.addDouble(tmp(1 , i));
            }
            
            if(verbose) printf("Sending %s:  %s\n", perfType.c_str(), bperf.toString().c_str());
            pred.write();
            
        }

        return true;
        
        //-----------------------------------
        //             Update
        //-----------------------------------
                    
        // Update estimator with a new input pair
        //if(verbose) std::cout << "Update # " << i+1 << std::endl;
        estimator.update(Xnew, ynew);
        if(verbose) cout << "Update completed" << endl;
    
    }

    /************************************************************************/
    bool interruptModule()
    {
        inVec.interrupt();
        printf("inVec interrupted\n");
        
        pred.interrupt();
        printf("pred interrupted\n");
        
        perf.interrupt();
        printf("perf interrupted\n");
        
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

    ResourceFinder rf;
    rf.setVerbose(true);
    rf.setDefaultConfigFile("RRLSestimator_config.ini");
    rf.setDefaultContext("iRRLS");
    rf.setDefault("name","RRLSestimator");
    rf.configure(argc,argv);

    RRLSestimator mod;
    return mod.runModule(rf);
}

//-------------------------------------------------------------------------------------------------------------------------
