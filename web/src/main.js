import './style.css'

const ledButton = document.getElementById('led_button');
let ledEnable = false; // Initial state can be fetched from the server if needed
ledButton.addEventListener('click', async () => {
    
  try {
    const response = await fetch(ledEnable ? '/api/led/off' : '/api/led/on', { method: 'POST' });
    if (response.ok) {
      ledEnable = !ledEnable;
      ledButton.classList.toggle('fill');
    } else {
      console.error(response.statusText);
    }
  } catch (error) {
    console.error(error);
  }
});