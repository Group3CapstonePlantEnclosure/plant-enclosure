// firebase-config.js
import { initializeApp } from "https://www.gstatic.com/firebasejs/10.8.1/firebase-app.js";
import { getDatabase, ref, update, onValue, get } from "https://www.gstatic.com/firebasejs/10.8.1/firebase-database.js";

const firebaseConfig = {
  apiKey: "AIzaSyCNS_uT-T1k0S_T9Ofa7iK0l3Ciu9Embtc",
  authDomain: "plant-enclosure.firebaseapp.com",
  databaseURL: "https://plant-enclosure-default-rtdb.firebaseio.com",
  projectId: "plant-enclosure",
  storageBucket: "plant-enclosure.firebasestorage.app",
  messagingSenderId: "8601368106",
  appId: "1:8601368106:web:515c7bbac13a6324b73553",
  measurementId: "G-NBSVHWE9S6"
};

// Initialize Firebase
const app = initializeApp(firebaseConfig);
const db = getDatabase(app);

// Export these so our HTML files can use them!
export { db, ref, update, onValue, get };